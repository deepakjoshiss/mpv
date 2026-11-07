#!/bin/sh
# Fresh meson setup of this mpv checkout against this folder (see native.ini). Then: venv/bin/meson compile -C build/mpv
set -e
D="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$D/build/mpv"
cd "$D/.."
"$D/venv/bin/meson" setup "$D/build/mpv" --native-file "$D/native.ini" \
  -Dbuildtype=release -Dlibmpv=false -Dtests=false \
  -Dlua=luajit -Djavascript=disabled -Duchardet=enabled \
  -Dcocoa=enabled -Dcoreaudio=enabled -Davfoundation=enabled -Dgl=enabled -Dgl-cocoa=enabled \
  -Dvideotoolbox-gl=enabled -Dvideotoolbox-pl=enabled -Dvulkan=enabled -Dswift-build=enabled \
  -Dmacos-cocoa-cb=enabled -Dmacos-media-player=enabled \
  -Dcdda=disabled -Ddvdnav=disabled -Ddvda=disabled -Dlibbluray=disabled -Dlibarchive=disabled \
  -Dlcms2=disabled -Djpeg=disabled -Drubberband=disabled -Dzimg=disabled -Dsixel=disabled \
  -Dcaca=disabled -Dvapoursynth=disabled \
  -Dobjc_args="-Wno-error=deprecated -Wno-error=deprecated-declarations"
ln -sfn "$D/lib" "$D/build/mpv/lib"
