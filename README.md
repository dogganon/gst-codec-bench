# GStreamer encoder/decoder benchmark

Standalone C++17 throughput test for the system GStreamer installation. It uses
**two independent pipelines**: raw appsrc → encoder → parser → encoded appsink,
and compressed appsrc → decoder → decoded appsink. A bridge thread copies
compressed buffers and forwards their negotiated caps and timestamps. Frames
are pushed as fast as possible (not paced in real time). Throughput includes
memory conversions and buffer transfer, not just codec latency.

For reproducible local x86-64 builds, install Pixi and run:

```sh
pixi run test
```

This builds and runs a software H.264 encode/decode smoke test. The Pixi
configuration includes the GStreamer nvcodec plugin, but hardware support
requires accessible NVIDIA CUDA/video driver libraries and GPU devices. On
this host the plugin reports `Unable to initialize CUDA library`, so NVENC
and NVDEC cannot be tested here. Use the system Jetson plugins on the G1.

including memory conversions and buffering, not isolated codec latency.

Run either hardware codec independently on the G1:

```sh
./build/gst_codec_bench --mode encode --frames 300
./build/gst_codec_bench --mode decode --input sample.h264 --frames 300
```

Decode-only takes an existing Annex B H.264 elementary stream; it does not
instantiate an encoder. Use `--width`, `--height`, and `--fps` to match the
input stream. `--frames` sets the maximum number of decoded frames; if the
file ends earlier, the actual number is reported. The default `--mode both`
keeps the two-pipeline round-trip benchmark.


On the Unitree G1 Jetson Orin (JetPack GStreamer plugins):

```sh
sudo apt-get install build-essential cmake pkg-config libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev
cmake -S . -B build
cmake --build build -j
./build/gst_codec_bench --frames 600 --width 640 --height 480
```

Defaults: `nvv4l2h264enc`, `h264parse`, `nvv4l2decoder`,
`nvvidconv`, NVMM input to the encoder. Check availability via
`gst-inspect-1.0 nvv4l2h264enc nvv4l2decoder nvvidconv`.
Hardware plugins require NVIDIA's JetPack/L4T multimedia packages; installing
upstream GStreamer alone does not supply them.

On an ordinary Linux PC with the necessary GStreamer plugins:

```sh
./build/gst_codec_bench --software
```

Override individual factories, e.g. `--encoder nvv4l2h265enc
--parser h265parse --decoder nvv4l2decoder`. For software or other
system-memory encoders use `--system-memory --converter videoconvert`.
The benchmark reports submitted, encoder-input/output, parsed, bridged, decoder-
input, and decoded counters. Encoded byte counts are measured after the parser.
It does not test camera capture, bitstream quality, or single-frame latency.
An invalid plugin, negotiation failure, or stall causes a nonzero exit.
