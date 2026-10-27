#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct Options {
    std::string encoder = "nvv4l2h264enc";
    std::string decoder = "nvv4l2decoder";
    std::string parser = "h264parse";
    std::string converter = "nvvidconv";
    std::string mode = "both";
    std::string profile;
    std::string input_file;
    bool nvmm = true;
    int width = 640;
    int height = 480;
    int fps = 30;
    double pace_fps = 0.0;
    int frames = 300;
};

static void usage(const char* executable) {
    std::cout
        << "Usage: " << executable << " [options]\n"
        << "  --mode MODE       both (default), encode, or decode\n"
        << "  --input PATH      H.264 elementary stream for decode mode\n"
        << "  --encoder NAME    GStreamer encoder (default nvv4l2h264enc)\n"
        << "  --decoder NAME    GStreamer decoder (default nvv4l2decoder)\n"
        << "  --profile NAME    H.264 baseline, main, or high (encoder modes)\n"
        << "  --parser NAME     GStreamer parser (default h264parse)\n"
        << "  --converter NAME  Raw video converter (default nvvidconv)\n"
        << "  --system-memory   Use ordinary raw-video caps instead of NVMM\n"
        << "  --software        Select x264enc / avdec_h264 / videoconvert\n"
        << "  --width N         Width, even (default 640)\n"
        << "  --height N        Height, even (default 480)\n"
        << "  --fps N           Frame timestamps per second (default 30)\n"
        << "  --pace-fps N      Input pacing rate (default 0: unlimited)\n"
        << "  --frames N        Number of frames (default 300)\n"
        << "  --help            Show this message\n";
}

static int number(const char* s) {
    std::size_t parsed = 0;
    const int value = std::stoi(s, &parsed);
    if (s[parsed] != '\0' || value <= 0) {
        throw std::runtime_error("expected a positive integer: " + std::string(s));
    }
    return value;
}

static double parse_pace_fps(const std::string& value) {
    std::size_t consumed = 0;
    const double result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result) || result < 0.0)
        throw std::runtime_error("--pace-fps must be a nonnegative finite number");
    return result;
}

static void pace_input(double fps, std::chrono::steady_clock::time_point start, int index) {
    if (fps > 0.0) {
        const auto delay = std::chrono::duration<double>(static_cast<double>(index) / fps);
        std::this_thread::sleep_until(start +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay));
    }
}

static Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if (arg == "--software") {
            opts.encoder = "x264enc";
            opts.decoder = "avdec_h264";
            opts.parser = "h264parse";
            opts.converter = "videoconvert";
            opts.nvmm = false;
            continue;
        }
        if (arg == "--system-memory") {
            opts.nvmm = false;
            continue;
        }
        if (++i >= argc) {
            throw std::runtime_error("missing value for " + arg);
        }
        const std::string value = argv[i];
        if (arg == "--mode") opts.mode = value;
        else if (arg == "--input") opts.input_file = value;
        else if (arg == "--encoder") opts.encoder = value;
        else if (arg == "--profile") opts.profile = value;
        else if (arg == "--decoder") opts.decoder = value;
        else if (arg == "--parser") opts.parser = value;
        else if (arg == "--converter") opts.converter = value;
        else if (arg == "--width") opts.width = number(argv[i]);
        else if (arg == "--height") opts.height = number(argv[i]);
        else if (arg == "--fps") opts.fps = number(argv[i]);
        else if (arg == "--pace-fps") opts.pace_fps = parse_pace_fps(value);
        else if (arg == "--frames") opts.frames = number(argv[i]);
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (opts.width % 2 || opts.height % 2) {
        throw std::runtime_error("I420 requires even width and height");
    }
    if (opts.mode != "both" && opts.mode != "encode" && opts.mode != "decode")
        throw std::runtime_error("--mode must be both, encode or decode");
    if (!opts.profile.empty() && opts.profile != "baseline" &&
        opts.profile != "main" && opts.profile != "high")
        throw std::runtime_error("--profile must be baseline, main, or high");
    if (opts.mode == "decode" && !opts.profile.empty())
        throw std::runtime_error("--profile only applies to encoding");
    if (opts.mode == "decode" && opts.pace_fps > 0.0)
        throw std::runtime_error("--pace-fps is only supported for encode and both modes");
    if (opts.mode == "decode" && opts.input_file.empty())
        throw std::runtime_error("--mode decode requires --input PATH");
    return opts;
}

static std::string raw_caps(const Options& o) {
    return "video/x-raw,format=I420,width=" + std::to_string(o.width) +
           ",height=" + std::to_string(o.height) +
           ",framerate=" + std::to_string(o.fps) + "/1";
}

static std::string encoder_pipeline(const Options& o) {
    const std::string input_caps = o.nvmm
        ? "video/x-raw(memory:NVMM),format=I420"
        : "video/x-raw,format=I420";
    return "appsrc name=input format=time block=true caps=\"" + raw_caps(o) +
           "\" ! queue ! " + o.converter + " ! " + input_caps +
           " ! " + o.encoder + " name=encoder ! queue ! " + o.parser +
           " name=bitstream" +
           (o.profile.empty() || o.encoder == "nvv4l2h264enc" ? "" :
            " ! video/x-h264,profile=" + o.profile) +
           " ! appsink name=encoded sync=false";
}

static std::string decoder_pipeline(const Options& o) {
    return "appsrc name=compressed format=time block=true ! queue ! " +
           o.decoder + " name=decoder ! queue ! " + o.converter +
           " ! " + raw_caps(o) + " ! appsink name=output sync=false";
}

struct Metrics {
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> input_buffers{0};
    std::atomic<std::uint64_t> encoder_input{0};
    std::atomic<std::uint64_t> encoder_output{0};
    std::atomic<std::uint64_t> decoder_input{0};
    std::atomic<bool> eos_sent{false};
    std::atomic<std::uint64_t> decoded{0};
    std::atomic<std::uint64_t> encoded_bytes{0};
    std::atomic<std::uint64_t> encoded_buffers{0};
};

static GstPadProbeReturn count_buffer(GstPad*, GstPadProbeInfo* info, gpointer data) {
    if (GST_PAD_PROBE_INFO_BUFFER(info)) {
        ++*static_cast<std::atomic<std::uint64_t>*>(data);
    }
    return GST_PAD_PROBE_OK;
}

static void probe_pad(GstElement* element, const char* name,
                      std::atomic<std::uint64_t>& counter) {
    GstPad* pad = gst_element_get_static_pad(element, name);
    if (!pad) throw std::runtime_error(std::string("missing pad ") + name);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer, &counter, nullptr);
    gst_object_unref(pad);
}

static GstPadProbeReturn count_encoded(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto* metrics = static_cast<Metrics*>(data);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer) {
        metrics->encoded_bytes += gst_buffer_get_size(buffer);
        ++metrics->encoded_buffers;
    }
    return GST_PAD_PROBE_OK;
}

static std::string bus_error(GstBus* bus) {
    GstMessage* msg = gst_bus_pop_filtered(bus,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR));
    if (!msg) return {};
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(msg, &error, &debug);
    std::string result = error ? error->message : "unknown GStreamer error";
    if (debug) result += " (" + std::string(debug) + ")";
    if (error) g_error_free(error);
    g_free(debug);
    gst_message_unref(msg);
    return result;
}

static GstElement* create_pipeline(const std::string& description) {
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
    if (error) {
        const std::string message = error->message;
        g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        throw std::runtime_error("pipeline creation: " + message);
    }
    if (!pipeline) throw std::runtime_error("pipeline creation failed");
    return pipeline;
}

// Encode-only mode does not instantiate a decoder at all.
static void set_encoder_profile(GstElement* encoder, const Options& o) {
    if (o.profile.empty() || o.encoder != "nvv4l2h264enc") return;
    GParamSpec* spec = g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "profile");
    if (!spec || !G_IS_PARAM_SPEC_ENUM(spec))
        throw std::runtime_error("NVENC does not expose an enum profile property");
    GEnumClass* values = G_ENUM_CLASS(g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(spec)));
    bool matched = false;
    for (guint i = 0; i < values->n_values; ++i) {
        const GEnumValue& value = values->values[i];
        const std::string nick = value.value_nick ? value.value_nick : "";
        const std::string name = value.value_name ? value.value_name : "";
        if (g_ascii_strcasecmp(nick.c_str(), o.profile.c_str()) == 0 ||
            g_ascii_strcasecmp(name.c_str(), o.profile.c_str()) == 0) {
            g_object_set(encoder, "profile", value.value, nullptr);
            matched = true;
            break;
        }
    }
    g_type_class_unref(values);
    if (!matched)
        throw std::runtime_error("unsupported NVENC H.264 profile: " + o.profile);
}

static void run_encode_only(const Options& o) {
    GstElement* pipeline = create_pipeline(encoder_pipeline(o));
    GstElement* input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    GstElement* output = gst_bin_get_by_name(GST_BIN(pipeline), "encoded");
    GstBus* bus = gst_element_get_bus(pipeline);
    if (!input || !output || !bus) throw std::runtime_error("missing encoder elements");
    GstElement* encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
    set_encoder_profile(encoder, o);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("could not start encoder");
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<int> submitted{0};
    const auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        const std::size_t size = static_cast<std::size_t>(o.width) * o.height * 3 / 2;
        std::vector<std::uint8_t> frame(size, 128);
        for (int i = 0; i < o.frames && !stop; ++i) {
            pace_input(o.pace_fps, start, i);
            if (stop) break;
            std::fill_n(frame.begin(), static_cast<std::size_t>(o.width) * o.height,
                        static_cast<std::uint8_t>(16 + (i * 7) % 220));
            GstBuffer* b = gst_buffer_new_allocate(nullptr, size, nullptr);
            gst_buffer_fill(b, 0, frame.data(), size);
            GST_BUFFER_PTS(b) = gst_util_uint64_scale(i, GST_SECOND, o.fps);
            GST_BUFFER_DURATION(b) = gst_util_uint64_scale(1, GST_SECOND, o.fps);
            if (gst_app_src_push_buffer(GST_APP_SRC(input), b) != GST_FLOW_OK) {
                if (!stop) failed = true;
                return;
            }
            ++submitted;
        }
        if (!stop && gst_app_src_end_of_stream(GST_APP_SRC(input)) != GST_FLOW_OK)
            failed = true;
    });

    int received = 0;
    std::uint64_t bytes = 0;
    std::string error;
    auto last_progress = start;
    auto last_report = start;
    int last_report_frames = 0;
    while (received < o.frames) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output),
                                                         200 * GST_MSECOND);
        if (sample) {
            ++received;
            bytes += gst_buffer_get_size(gst_sample_get_buffer(sample));
            gst_sample_unref(sample);
            last_progress = std::chrono::steady_clock::now();
        }
        error = bus_error(bus);
        if (!error.empty() || failed || gst_app_sink_is_eos(GST_APP_SINK(output))) break;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            const double interval = std::chrono::duration<double>(now - last_report).count();
            std::cerr << "submitted=" << submitted << " encoded=" << received
                      << "/" << o.frames << " throughput="
                      << (received - last_report_frames) / interval << " fps\n";
            last_report_frames = received;
            last_report = now;
        }
        if (now - last_progress > std::chrono::seconds(10)) {
            error = "encoder stalled for 10s";
            break;
        }
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    stop = true;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    producer.join();
    if (error.empty()) error = bus_error(bus);
    std::cout << "Encoded: " << received << "/" << o.frames << " frames, "
              << bytes << " bytes, " << received / elapsed << " fps\n";
    gst_object_unref(bus);
    gst_object_unref(input);
    gst_object_unref(output);
    gst_object_unref(encoder);
    gst_object_unref(pipeline);
    if (!error.empty()) throw std::runtime_error(error);
    if (failed || received != o.frames) throw std::runtime_error("encode incomplete");
}

// Decode-only mode uses an existing raw H.264 Annex B file, without NVENC.
static void run_decode_only(const Options& o) {
    const std::string description =
        "filesrc name=source ! " + o.parser + " ! " + o.decoder +
        " name=decoder ! queue ! " + o.converter + " ! " + raw_caps(o) +
        " ! appsink name=output sync=false";
    GstElement* pipeline = create_pipeline(description);
    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    GstElement* output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    GstBus* bus = gst_element_get_bus(pipeline);
    if (!source || !output || !bus) throw std::runtime_error("missing decoder elements");
    g_object_set(source, "location", o.input_file.c_str(), nullptr);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("could not start decoder");
    const auto start = std::chrono::steady_clock::now();
    auto last_progress = start;
    auto last_report = start;
    int decoded = 0;
    int last_report_frames = 0;
    std::string error;
    while (decoded < o.frames) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output),
                                                         200 * GST_MSECOND);
        if (sample) {
            ++decoded;
            gst_sample_unref(sample);
            last_progress = std::chrono::steady_clock::now();
        }
        error = bus_error(bus);
        if (!error.empty() || gst_app_sink_is_eos(GST_APP_SINK(output))) break;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            const double interval = std::chrono::duration<double>(now - last_report).count();
            std::cerr << "decoded=" << decoded << "/" << o.frames
                      << " throughput=" << (decoded - last_report_frames) / interval
                      << " fps\n";
            last_report_frames = decoded;
            last_report = now;
        }
        if (now - last_progress > std::chrono::seconds(10)) {
            error = "decoder stalled for 10s";
            break;
        }
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (error.empty()) error = bus_error(bus);
    std::cout << "Decoded: " << decoded << " frames in " << elapsed
              << " s, " << decoded / elapsed << " fps\n";
    gst_object_unref(bus);
    gst_object_unref(source);
    gst_object_unref(output);
    gst_object_unref(pipeline);
    if (!error.empty()) throw std::runtime_error(error);
    if (!decoded) throw std::runtime_error("no decoded frames");
}

static void run(const Options& o) {
    const std::string enc_description = encoder_pipeline(o);
    const std::string dec_description = decoder_pipeline(o);
    std::cout << "Encoder pipeline: " << enc_description << "\n"
              << "Decoder pipeline: " << dec_description << std::endl;

    GstElement* enc = create_pipeline(enc_description);
    GstElement* dec = create_pipeline(dec_description);
    GstElement* input = gst_bin_get_by_name(GST_BIN(enc), "input");
    GstElement* encoded = gst_bin_get_by_name(GST_BIN(enc), "encoded");
    GstElement* parser = gst_bin_get_by_name(GST_BIN(enc), "bitstream");
    GstElement* encoder = gst_bin_get_by_name(GST_BIN(enc), "encoder");
    GstElement* compressed = gst_bin_get_by_name(GST_BIN(dec), "compressed");
    GstElement* output = gst_bin_get_by_name(GST_BIN(dec), "output");
    GstElement* decoder = gst_bin_get_by_name(GST_BIN(dec), "decoder");
    GstBus* enc_bus = gst_element_get_bus(enc);
    GstBus* dec_bus = gst_element_get_bus(dec);
    if (!input || !encoded || !parser || !encoder || !compressed ||
        !output || !decoder || !enc_bus || !dec_bus) {
        throw std::runtime_error("required pipeline element missing");
    }

    set_encoder_profile(encoder, o);
    Metrics metrics;
    probe_pad(input, "src", metrics.input_buffers);
    probe_pad(encoder, "sink", metrics.encoder_input);
    probe_pad(encoder, "src", metrics.encoder_output);
    probe_pad(decoder, "sink", metrics.decoder_input);
    GstPad* parser_pad = gst_element_get_static_pad(parser, "src");
    gst_pad_add_probe(parser_pad, GST_PAD_PROBE_TYPE_BUFFER, count_encoded, &metrics, nullptr);
    gst_object_unref(parser_pad);

    if (gst_element_set_state(dec, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
        gst_element_set_state(enc, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        throw std::runtime_error("failed to start GStreamer pipelines");
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> producer_failed{false};
    std::atomic<bool> bridge_failed{false};
    std::atomic<std::uint64_t> bridged{0};
    const auto start = std::chrono::steady_clock::now();

    std::thread producer([&] {
        const std::size_t frame_size = static_cast<std::size_t>(o.width) * o.height * 3 / 2;
        std::vector<std::uint8_t> frame(frame_size, 128);
        for (int i = 0; i < o.frames && !stop; ++i) {
            pace_input(o.pace_fps, start, i);
            if (stop) break;
            std::fill_n(frame.begin(), static_cast<std::size_t>(o.width) * o.height,
                        static_cast<std::uint8_t>(16 + (i * 7) % 220));
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame_size, nullptr);
            gst_buffer_fill(buffer, 0, frame.data(), frame_size);
            GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(i, GST_SECOND, o.fps);
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, o.fps);
            if (gst_app_src_push_buffer(GST_APP_SRC(input), buffer) != GST_FLOW_OK) {
                if (!stop) producer_failed = true;
                return;
            }
            ++metrics.submitted;
        }
        if (!stop && gst_app_src_end_of_stream(GST_APP_SRC(input)) == GST_FLOW_OK) {
            metrics.eos_sent = true;
        } else if (!stop) {
            producer_failed = true;
        }
    });

    // Transfer compressed buffers through ordinary system memory. The two
    // GStreamer pipelines own independent encoder/decoder state and allocation.
    std::thread bridge([&] {
        bool have_caps = false;
        while (!stop) {
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(encoded),
                                                              200 * GST_MSECOND);
            if (sample) {
                GstCaps* caps = gst_sample_get_caps(sample);
                if (!caps) {
                    bridge_failed = true;
                    gst_sample_unref(sample);
                    break;
                }
                if (!have_caps) {
                    gst_app_src_set_caps(GST_APP_SRC(compressed), caps);
                    have_caps = true;
                }
                // Copy encoded bytes so the decoder does not hold buffers from
                // the NVIDIA encoder's pool.
                GstBuffer* copy = gst_buffer_copy_deep(gst_sample_get_buffer(sample));
                gst_sample_unref(sample);
                if (!copy || gst_app_src_push_buffer(GST_APP_SRC(compressed), copy) != GST_FLOW_OK) {
                    if (!stop) bridge_failed = true;
                    break;
                }
                ++bridged;
            } else if (gst_app_sink_is_eos(GST_APP_SINK(encoded))) {
                if (gst_app_src_end_of_stream(GST_APP_SRC(compressed)) != GST_FLOW_OK && !stop)
                    bridge_failed = true;
                break;
            }
        }
    });

    int decoded = 0;
    std::string failure;
    auto last_report = start;
    auto last_progress = start;
    std::uint64_t last_total = 0;
    while (decoded < o.frames) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output),
                                                        200 * GST_MSECOND);
        if (sample) {
            ++decoded;
            ++metrics.decoded;
            gst_sample_unref(sample);
        }
        if (producer_failed || bridge_failed) {
            failure = "producer or compressed-buffer bridge failed";
            break;
        }
        failure = bus_error(enc_bus);
        if (!failure.empty()) { failure = "encoder: " + failure; break; }
        failure = bus_error(dec_bus);
        if (!failure.empty()) { failure = "decoder: " + failure; break; }
        if (gst_app_sink_is_eos(GST_APP_SINK(output))) break;
        const auto now = std::chrono::steady_clock::now();
        const auto total = metrics.submitted.load() + metrics.input_buffers.load() +
                           metrics.encoder_input.load() + metrics.encoder_output.load() +
                           metrics.encoded_buffers.load() + bridged.load() +
                           metrics.decoder_input.load() + metrics.decoded.load();
        if (total != last_total) {
            last_progress = now;
            last_total = total;
        }
        if (now - last_report >= std::chrono::seconds(1)) {
            const double seconds = std::chrono::duration<double>(now - start).count();
            std::cerr << "[" << seconds << "s] submitted=" << metrics.submitted.load()
                      << " input=" << metrics.input_buffers.load()
                      << " enc_in=" << metrics.encoder_input.load()
                      << " enc_out=" << metrics.encoder_output.load()
                      << " parsed=" << metrics.encoded_buffers.load()
                      << " bridged=" << bridged.load()
                      << " dec_in=" << metrics.decoder_input.load()
                      << " eos_sent=" << metrics.eos_sent.load()
                      << " decoded=" << decoded << "/" << o.frames
                      << " throughput=" << decoded / seconds << " fps\n";
            last_report = now;
        }
        if (now - last_progress > std::chrono::seconds(10)) {
            failure = "pipeline stalled for 10s";
            break;
        }
        if (now - start > std::chrono::seconds(120)) {
            failure = "pipeline timed out after 120s";
            break;
        }
    }
    const auto finish = std::chrono::steady_clock::now();
    stop = true;
    // Unblock appsrc push operations and pending appsink pulls before joining.
    gst_element_set_state(enc, GST_STATE_NULL);
    gst_element_set_state(dec, GST_STATE_NULL);
    producer.join();
    bridge.join();
    if (failure.empty()) failure = bus_error(enc_bus);
    if (failure.empty()) failure = bus_error(dec_bus);

    const double seconds = std::chrono::duration<double>(finish - start).count();
    std::cout << "Decoded: " << decoded << "/" << o.frames
              << " frames, elapsed: " << seconds << " s, throughput: "
              << decoded / seconds << " fps\n"
              << "Encoded: " << metrics.encoded_buffers.load() << " buffers, "
              << metrics.encoded_bytes.load() << " bytes, "
              << (metrics.encoded_bytes.load() * 8.0 / seconds / 1e6)
              << " Mbit/s (wall-clock rate)\n";

    gst_object_unref(enc_bus);
    gst_object_unref(dec_bus);
    gst_object_unref(parser);
    gst_object_unref(encoder);
    gst_object_unref(decoder);
    gst_object_unref(output);
    gst_object_unref(encoded);
    gst_object_unref(compressed);
    gst_object_unref(input);
    gst_object_unref(enc);
    gst_object_unref(dec);

    if (!failure.empty()) throw std::runtime_error(failure);
    if (decoded != o.frames) throw std::runtime_error("incomplete encode/decode");
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    try {
        const Options opts = parse_options(argc, argv);
        if (opts.mode == "encode") run_encode_only(opts);
        else if (opts.mode == "decode") run_decode_only(opts);
        else run(opts);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
