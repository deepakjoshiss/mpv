#!/bin/sh
# Compiles ~/work/apps/mpv and rebuilds "~/Applications/mpv Native.app" from it, so the app is never stale.
# On a fresh checkout or after a meson.build/options change, run setup-mpv.sh first.
set -e
D="$(cd "$(dirname "$0")" && pwd)"
PATH="$D/venv/bin:$PATH" meson compile -C "$D/build/mpv"
"$D/make-app.sh"
