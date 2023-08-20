#!/bin/sh -e

meson setup build            \
  -D cdda=enabled            \
  -D d3d-hwaccel=enabled     \
  -D d3d11=enabled           \
  -D dvdnav=enabled          \
  -D egl-angle-lib=enabled   \
  -D egl-angle-win32=enabled \
  -D jpeg=enabled            \
  -D lcms2=enabled           \
  -D libarchive=enabled      \
  -D libbluray=enabled       \
  -D libmpv=true             \
  -D libplacebo=enabled      \
  -D lua=enabled             \
  -D pdf-build=enabled       \
  -D rubberband=enabled      \
  -D shaderc=enabled         \
  -D spirv-cross=enabled     \
  -D tests=true              \
  -D uchardet=enabled        \
  -D vapoursynth=enabled     \
  -D vulkan=enabled
meson compile -C build
cp ./build/player/DJmpv.com ./build
./build/DJmpv.com -v --no-config
