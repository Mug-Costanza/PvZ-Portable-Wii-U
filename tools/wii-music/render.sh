#!/bin/sh
# Builds and runs the Wii music pre-renderer (see render_music.cpp).
# Needs libopenmpt and libvorbis (e.g. `brew install libopenmpt libvorbis`).
#
# Usage: tools/wii-music/render.sh <main.pak> [output dir]
# Then copy the output dir to the SD card as
#   sd:/apps/PvZPortable/sounds/prerendered/
set -e

PAK="${1:?usage: $0 <main.pak> [output dir]}"
OUT="${2:-prerendered}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${TMPDIR:-/tmp}/pvz-render-music"

c++ -std=c++17 -O2 "$HERE/render_music.cpp" -o "$BIN" \
	$(pkg-config --cflags --libs libopenmpt vorbisenc vorbis ogg)
mkdir -p "$OUT"
"$BIN" "$PAK" "$OUT"
