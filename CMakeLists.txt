cmake_minimum_required(VERSION 3.16)
project(gst_codec_bench LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

add_executable(gst_codec_bench main.cpp)
target_compile_features(gst_codec_bench PRIVATE cxx_std_17)
target_link_libraries(gst_codec_bench PRIVATE PkgConfig::GST Threads::Threads)
target_compile_options(gst_codec_bench PRIVATE -Wall -Wextra -Wpedantic)
