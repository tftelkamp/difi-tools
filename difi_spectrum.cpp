#include <zmq.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>

#include <chrono>
// #include <complex>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

// VRT
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <vrt/vrt_read.h>
#include <vrt/vrt_string.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>

#include <complex.h>
#include <fftw3.h>

namespace po = boost::program_options;

#define NUM_POINTS 10000
#define REAL 0
#define IMAG 1

static bool stop_signal_called = false;
void sig_int_handler(int)
{
    stop_signal_called = true;
}

template <typename samp_type> inline float get_abs_val(samp_type t)
{
    return std::fabs(t);
}

inline float get_abs_val(std::complex<int16_t> t)
{
    return std::fabs(t.real());
}

inline float get_abs_val(std::complex<int8_t> t)
{
    return std::fabs(t.real());
}

int main(int argc, char* argv[])
{

    // FFTW
    fftw_complex *signal, *result;
    fftw_plan plan;
    uint32_t num_points = 0;
    uint32_t integrate = 0;

    int64_t rf_freq = 0;
    uint32_t rate = 0;

    // variables to be set by po
    std::string file, type, zmq_address;
    size_t num_requested_samples;
    uint32_t bins, updates_per_second;
    double total_time;
    uint16_t port;
    int hwm;

    // setup the program options
    po::options_description desc("Allowed options");
    // clang-format off

    desc.add_options()
        ("help", "help message")
        // ("file", po::value<std::string>(&file)->default_value("usrp_samples.dat"), "name of the file to write binary samples to")
        // ("type", po::value<std::string>(&type)->default_value("short"), "sample type: double, float, or short")
        ("nsamps", po::value<size_t>(&num_requested_samples)->default_value(0), "total number of samples to receive")
        ("duration", po::value<double>(&total_time)->default_value(0), "total number of seconds to receive")
        ("progress", "periodically display short-term bandwidth")
        // ("stats", "show average bandwidth on exit")
        ("int-second", "align start of reception to integer second")
        ("bins", po::value<uint32_t>(&bins)->default_value(100), "Spectrum bins (default 100)")
        ("updates", po::value<uint32_t>(&updates_per_second)->default_value(1), "Updates per second (default 1)")
        ("null", "run without writing to file")
        ("continue", "don't abort on a bad packet")
        ("address", po::value<std::string>(&zmq_address)->default_value("localhost"), "DIFI ZMQ address")
        ("port", po::value<uint16_t>(&port)->default_value(50100), "DIFI ZMQ port")
        ("hwm", po::value<int>(&hwm)->default_value(10000), "DIFI ZMQ HWM")

    ;
    // clang-format on
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    // print the help message
    if (vm.count("help")) {
        std::cout << boost::format("DIFI samples to gnuplot %s") % desc << std::endl;
        std::cout << std::endl
                  << "This application streams data from a DIFI stream "
                     "to gnuplot.\n"
                  << std::endl;
        return ~0;
    }

    bool progress               = vm.count("progress") > 0;
    bool stats                  = vm.count("stats") > 0;
    bool null                   = vm.count("null") > 0;
    bool continue_on_bad_packet = vm.count("continue") > 0;
    bool int_second             = (bool)vm.count("int-second");

    std::vector<size_t> channel_nums = {0}; // single channel (0)

    // ZMQ

    void *context = zmq_ctx_new();
    void *subscriber = zmq_socket(context, ZMQ_SUB);
    int rc = zmq_setsockopt (subscriber, ZMQ_RCVHWM, &hwm, sizeof hwm);
    std::string connect_string = "tcp://" + zmq_address + ":" + std::to_string(port);
    rc = zmq_connect(subscriber, connect_string.c_str());
    assert(rc == 0);
    zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "", 0);

    bool first_frame = true;

    // time keeping
    auto start_time = std::chrono::steady_clock::now();
    auto stop_time =
        start_time + std::chrono::milliseconds(int64_t(1000 * total_time));

    int8_t packet_count = -1;

    uint32_t buffer[100000];

    struct vrt_header h;
    struct vrt_fields f;
    
    unsigned long long num_total_samps = 0;

    // Track time and samps between updating the BW summary
    auto last_update                     = start_time;
    unsigned long long last_update_samps = 0;

    bool start_rx = false;
    uint64_t last_fractional_seconds_timestamp = 0;

    uint32_t signal_pointer = 0;

    while (not stop_signal_called
           and (num_requested_samples > num_total_samps or num_requested_samples == 0)
           and (total_time == 0.0 or std::chrono::steady_clock::now() <= stop_time)) {

        int len = zmq_recv(subscriber, buffer, 100000, 0);

        const auto now = std::chrono::steady_clock::now();

        int32_t offset = 0;
        int32_t rv = vrt_read_header(buffer + offset, 100000 - offset, &h, true);

        /* Parse header */
        if (rv < 0) {
            fprintf(stderr, "Failed to parse header: %s\n", vrt_string_error(rv));
            return EXIT_FAILURE;
        }
        offset += rv;

        if (not start_rx and (h.packet_type == VRT_PT_IF_CONTEXT)) {
            start_rx = true;
            // printf("Packet type: %s\n", vrt_string_packet_type(h.packet_type));

            /* Parse fields */
            rv = vrt_read_fields(&h, buffer + offset, 100000 - offset, &f, true);
            if (rv < 0) {
                fprintf(stderr, "Failed to parse fields section: %s\n", vrt_string_error(rv));
                return EXIT_FAILURE;
            }
            offset += rv;

            struct vrt_if_context c;
            rv = vrt_read_if_context(buffer + offset, 100000 - offset, &c, true);
            if (rv < 0) {
                fprintf(stderr, "Failed to parse IF context section: %s\n", vrt_string_error(rv));
                return EXIT_FAILURE;
            }
            if (c.has.sample_rate) {
                // printf("Sample Rate [samples per second]: %.0f\n", c.sample_rate);
                rate = (uint32_t)c.sample_rate;
            } else {
                printf("No Rate\n");
            }
            if (c.has.rf_reference_frequency) {
                // printf("RF Freq [Hz]: %.0f\n", c.rf_reference_frequency);
                rf_freq = (int64_t)round(c.rf_reference_frequency);
            } else {
                printf("No Freq\n");
            }
            if (rate and rf_freq) {
                num_points = rate/updates_per_second;
                signal = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * num_points);
                result = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * num_points);
                plan = fftw_plan_dft_1d(num_points, signal, result, FFTW_FORWARD, FFTW_ESTIMATE);

                integrate = num_points/bins;

                printf("# Center freq: %li, rate: %u\n", rf_freq, rate);
                printf("# timestamp ");
                for (uint32_t i = 0; i < bins; ++i) {
                    printf(", %.0f", (double)(rf_freq + (i+0.5)*integrate - rate/2));
                }
                printf("\n");

            } else {
                printf("Context received but no rate and frequency. Exiting.\n");
                break;
            }

        }

        if (start_rx and (h.packet_type == VRT_PT_IF_DATA_WITH_STREAM_ID)) {

            if (not first_frame and (h.packet_count != (packet_count+1)%16) ) {
                printf("Error: lost frame (expected %i, received %i)\n", packet_count, h.packet_count);
                if (not continue_on_bad_packet)
                    break;
                else
                    packet_count = h.packet_count;
            } else {
                packet_count = h.packet_count;
            }

            /* Parse fields */
            rv = vrt_read_fields(&h, buffer + offset, 100000 - offset, &f, true);
            if (rv < 0) {
                fprintf(stderr, "Failed to parse fields section: %s\n", vrt_string_error(rv));
                return EXIT_FAILURE;
            }
            offset += rv;

            if (int_second) {
                // check if fractional second has wrapped
                if (f.fractional_seconds_timestamp > last_fractional_seconds_timestamp ) {
                        last_fractional_seconds_timestamp = f.fractional_seconds_timestamp;
                        continue;
                } else {
                    int_second = false;
                }
            }

            uint32_t num_rx_samps = (h.packet_size-offset);

            int mult = 1;
            for (uint32_t i = 0; i < 10000; i++) {
                signal_pointer++;
                if (signal_pointer >= num_points)  
                    break;
                int16_t re;
                memcpy(&re, (char*)&buffer[offset+i], 2);
                int16_t img;
                memcpy(&img, (char*)&buffer[offset+i]+2, 2);
                signal[signal_pointer][REAL] = mult*re;
                signal[signal_pointer][IMAG] = mult*img;
                mult *= -1;
            }

            if (signal_pointer >= num_points) {

                signal_pointer = 0;

                fftw_execute(plan);

                double avg = 0;
 
                uint64_t seconds = f.integer_seconds_timestamp;
                uint64_t frac_seconds = f.fractional_seconds_timestamp;
                frac_seconds += 10000*1e12/rate;
                if (frac_seconds > 1e12) {
                    frac_seconds -= 1e12;
                    seconds++;
                }

                printf("%lu.%09li", seconds, (int64_t)(frac_seconds/1e3));

                for (uint32_t i = 0; i < num_points; ++i) {
                    double mag = sqrt(result[i][REAL] * result[i][REAL] +
                              result[i][IMAG] * result[i][IMAG]);
                    avg += mag/(double)integrate;
                    if (i % integrate == integrate-1) {
                        printf(", %.3f",20*log10(avg/(double)num_points));
                        avg = 0;
                    }
                }

                printf("\n");
                fflush(stdout);

            }

            num_total_samps += num_rx_samps;

        }

        if (progress) {
            last_update_samps += (h.packet_size-offset);
            const auto time_since_last_update = now - last_update;
            if (time_since_last_update > std::chrono::seconds(1)) {
                const double time_since_last_update_s =
                    std::chrono::duration<double>(time_since_last_update).count();
                const double rate = double(last_update_samps) / time_since_last_update_s;
                std::cout << "\t" << (rate / 1e6) << " Msps, ";
                
                last_update_samps = 0;
                last_update       = now;

                uint32_t num_rx_samps = (h.packet_size-offset);
    
                float sum_i = 0;
                uint32_t clip_i = 0;

                double datatype_max = 32768.;
                // if (cpu_format == "sc8" || cpu_format == "s8")
                //     datatype_max = 128.;

                for (int i=0; i<num_rx_samps; i++ ) {
                    auto sample_i = get_abs_val((std::complex<int16_t>)buffer[offset+i]);
                    sum_i += sample_i;
                    if (sample_i > datatype_max*0.99)
                        clip_i++;
                }
                sum_i = sum_i/num_rx_samps;
                std::cout << boost::format("%.0f") % (100.0*log2(sum_i)/log2(datatype_max)) << "% I (";
                std::cout << boost::format("%.0f") % ceil(log2(sum_i)+1) << " of ";
                std::cout << (int)ceil(log2(datatype_max)+1) << " bits), ";
                std::cout << "" << boost::format("%.0f") % (100.0*clip_i/num_rx_samps) << "% I clip, ";
                std::cout << std::endl;

            }
        }
    }

    zmq_close(subscriber);
    zmq_ctx_destroy(context);

    return 0;

}  
