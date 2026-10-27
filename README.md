# GStreamer encoder/decoder benchmark

Standalone C++17 throughput test for the system GStreamer installation. It pushes
synthetic I420 frames through an encoder, parser, decoder, and appsink. The frame
timestamps represent the configured input FPS, but input is pushed **as fast as
possible** (not paced in real time). Throughput measures the full pipeline,
including memory conversions and buffering, not isolated codec latency.

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
Use `--help` for all options.

The benchmark counts encoded buffers/bytes at the parser output, and decoded
frames at the final appsink. It does not test camera capture, codec bitstream
quality, or single-frame latency. An invalid/missing plugin or unsupported
caps negotiation causes a nonzero exit and a GStreamer diagnostic.
