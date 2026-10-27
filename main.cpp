#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
    bool nvmm = true;
    int width = 640;
    int height = 480;
    int fps = 30;
    int frames = 300;
};

static void usage(const char* executable) {
    std::cout
        << "Usage: " << executable << " [options]\n"
        << "  --encoder NAME    GStreamer encoder (default nvv4l2h264enc)\n"
        << "  --decoder NAME    GStreamer decoder (default nvv4l2decoder)\n"
        << "  --parser NAME     GStreamer parser (default h264parse)\n"
        << "  --converter NAME  Raw video converter (default nvvidconv)\n"
        << "  --system-memory   Use ordinary raw-video caps instead of NVMM\n"
        << "  --software        Select x264enc / avdec_h264 / videoconvert\n"
        << "  --width N         Width, even (default 640)\n"
        << "  --height N        Height, even (default 480)\n"
        << "  --fps N           Frame timestamps per second (default 30)\n"
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
        if (arg == "--encoder") opts.encoder = value;
        else if (arg == "--decoder") opts.decoder = value;
        else if (arg == "--parser") opts.parser = value;
        else if (arg == "--converter") opts.converter = value;
        else if (arg == "--width") opts.width = number(argv[i]);
        else if (arg == "--height") opts.height = number(argv[i]);
        else if (arg == "--fps") opts.fps = number(argv[i]);
        else if (arg == "--frames") opts.frames = number(argv[i]);
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (opts.width % 2 || opts.height % 2) {
        throw std::runtime_error("I420 requires even width and height");
    }
    return opts;
}

static std::string make_pipeline(const Options& o) {
    const std::string caps = "video/x-raw,format=I420,width=" +
        std::to_string(o.width) + ",height=" + std::to_string(o.height) +
        ",framerate=" + std::to_string(o.fps) + "/1";
    const std::string encoded_input = o.nvmm
        ? "video/x-raw(memory:NVMM),format=I420"
        : "video/x-raw,format=I420";
    return "appsrc name=input format=time is-live=false block=true caps=\"" + caps +
        "\" ! queue ! " + o.converter + " ! " + encoded_input +
        " ! " + o.encoder + " ! " + o.parser +
        " name=bitstream ! " + o.decoder + " ! " + o.converter +
        " ! " + caps + " ! appsink name=output sync=false";
}

struct Metrics {
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> input_buffers{0};
    std::atomic<std::uint64_t> decoded{0};
    std::atomic<std::uint64_t> encoded_bytes{0};
    std::atomic<std::uint64_t> encoded_buffers{0};
};

static GstPadProbeReturn count_input(GstPad*, GstPadProbeInfo* info, gpointer data) {
    if (GST_PAD_PROBE_INFO_BUFFER(info)) {
        ++static_cast<Metrics*>(data)->input_buffers;
    }
    return GST_PAD_PROBE_OK;
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

static void run(const Options& o) {
    GError* error = nullptr;
    const std::string description = make_pipeline(o);
    std::cout << "Pipeline: " << description << "\n";
    GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
    if (error) {
        const std::string message = error->message;
        g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        throw std::runtime_error("pipeline creation: " + message);
    }
    if (!pipeline) throw std::runtime_error("pipeline creation failed");

    GstElement* input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    GstElement* output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    GstElement* parser = gst_bin_get_by_name(GST_BIN(pipeline), "bitstream");
    GstBus* bus = gst_element_get_bus(pipeline);
    if (!input || !output || !parser || !bus) {
        throw std::runtime_error("required pipeline element missing");
    }

    Metrics metrics;
    GstPad* input_pad = gst_element_get_static_pad(input, "src");
    gst_pad_add_probe(input_pad, GST_PAD_PROBE_TYPE_BUFFER, count_input, &metrics, nullptr);
    gst_object_unref(input_pad);
    GstPad* pad = gst_element_get_static_pad(parser, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count_encoded, &metrics, nullptr);
    gst_object_unref(pad);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        throw std::runtime_error("failed to start pipeline: " + bus_error(bus));
    }

    std::atomic<bool> failed{false};
    const auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        const std::size_t frame_size =
            static_cast<std::size_t>(o.width) * o.height * 3 / 2;
        std::vector<std::uint8_t> frame(frame_size, 128);
        for (int i = 0; i < o.frames && !failed; ++i) {
            // Changing luminance prevents trivial identical-frame encoding.
            std::fill_n(frame.begin(), static_cast<std::size_t>(o.width) * o.height,
                        static_cast<std::uint8_t>(16 + (i * 7) % 220));
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame_size, nullptr);
            gst_buffer_fill(buffer, 0, frame.data(), frame_size);
            GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(i, GST_SECOND, o.fps);
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, o.fps);
            if (gst_app_src_push_buffer(GST_APP_SRC(input), buffer) != GST_FLOW_OK) {
                failed = true;
                return;
            }
            ++metrics.submitted;
        }
        if (!failed) {
            if (gst_app_src_end_of_stream(GST_APP_SRC(input)) != GST_FLOW_OK) {
                failed = true;
            }
        }
    });

    int decoded = 0;
    std::string failure;
    auto last_report = start;
    auto last_progress = start;
    std::uint64_t last_total = 0;
    while (decoded < o.frames && !failed) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output),
                                                        200 * GST_MSECOND);
        if (sample) {
            ++decoded;
            ++metrics.decoded;
            gst_sample_unref(sample);
        }
        failure = bus_error(bus);
        if (!failure.empty()) break;
        if (gst_app_sink_is_eos(GST_APP_SINK(output))) break;
        const auto now = std::chrono::steady_clock::now();
        const auto total = metrics.submitted.load() + metrics.input_buffers.load() +
                           metrics.encoded_buffers.load() + metrics.decoded.load();
        if (total != last_total) {
            last_progress = now;
            last_total = total;
        }
        if (now - last_report >= std::chrono::seconds(1)) {
            const double seconds = std::chrono::duration<double>(now - start).count();
            std::cerr << "[" << seconds << "s] submitted=" << metrics.submitted.load()
                      << " input=" << metrics.input_buffers.load()
                      << " encoded=" << metrics.encoded_buffers.load()
                      << " decoded=" << decoded << "/" << o.frames
                      << " throughput=" << decoded / seconds << " fps\n";
            last_report = now;
        }
        if (now - last_progress > std::chrono::seconds(10)) {
            failure = "pipeline stalled for 10s (inspect submitted/input/encoded/decoded counters)";
            break;
        }
        if (now - start > std::chrono::seconds(120)) {
            failure = "pipeline timed out after 120s";
            break;
        }
    }
    const auto stop = std::chrono::steady_clock::now();
    // Flush blocked appsrc before joining the producer on any failure.
    if (decoded != o.frames || failed || !failure.empty()) {
        failed = true;
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    producer.join();
    if (failure.empty()) failure = bus_error(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);

    const double seconds = std::chrono::duration<double>(stop - start).count();
    std::cout << "Decoded: " << decoded << "/" << o.frames
              << " frames, elapsed: " << seconds << " s, throughput: "
              << decoded / seconds << " fps\n"
              << "Encoded: " << metrics.encoded_buffers.load() << " buffers, "
              << metrics.encoded_bytes.load() << " bytes, "
              << (metrics.encoded_bytes.load() * 8.0 / seconds / 1e6)
              << " Mbit/s (wall-clock rate)\n";

    gst_object_unref(bus);
    gst_object_unref(parser);
    gst_object_unref(output);
    gst_object_unref(input);
    gst_object_unref(pipeline);

    if (!failure.empty()) throw std::runtime_error(failure);
    if (failed || decoded != o.frames) {
        throw std::runtime_error("encode/decode did not complete all frames");
    }
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    try {
        run(parse_options(argc, argv));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
