#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# build.sh [out dir]: builds cqconsole.so (i386) for the official Linux Costume Quest.
#   CLANG=<clang>  any clang with the x86 target and ld.lld next to it (default: clang on PATH)
#   FONT=<ttf>     a monospace TrueType font (default: DejaVu Sans Mono); needs ffmpeg with drawtext
# Needs no i386 C headers: only the system's 32-bit libc.so.6 (LIB32, default /usr/lib32) to link with.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
CLANG=${CLANG:-clang}
LIB32=${LIB32:-/usr/lib32}
FONT=${FONT:-$(fc-match -f '%{file}' 'DejaVu Sans Mono' 2>/dev/null || echo /usr/share/fonts/TTF/DejaVuSansMono.ttf)}
mkdir -p "$OUT"
python3 "$HERE/make_font.py" "$FONT" "$OUT/font.h"
"$CLANG" --target=i386-unknown-linux-gnu -O2 -Wall -fPIC -fno-stack-protector -mstackrealign \
    -ffreestanding -fno-builtin -I"$OUT" -shared -nostdlib -fuse-ld=lld -Wl,--no-undefined \
    -o "$OUT/cqconsole.so" "$HERE/cqconsole.c" "$LIB32/libc.so.6"
cp "$HERE/run.sh" "$OUT/"
echo "Built $OUT/cqconsole.so"
