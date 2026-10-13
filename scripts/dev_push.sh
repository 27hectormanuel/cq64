#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Development loop: rebuild libcqhost for the connected device's ABI and push it (plus the
# guest libraries) into the app's files/dev/ directory, which HostLoader prefers.
# Needs a previous scripts/build_apk.sh for that ABI (it configures build/<abi>).
#   scripts/dev_push.sh [--run]
set -euo pipefail
source "$(dirname "$0")/env.sh"
HERE=$ROOT
PKG=com.doublefine.cq

abi=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
bdir=$HERE/build/$abi
NDK=$NDK "$HERE/guest/build.sh" "$HERE/out" >/dev/null
cmake --build "$bdir" -j"$(nproc)" | grep -E "error|FAILED" -A5 || true
[ -f "$bdir/libcqhost.so" ] || exit 1

tmp=/data/local/tmp/cqdev
adb shell "mkdir -p $tmp"
adb push "$bdir/libcqhost.so" "$tmp/libcqhost.so" >/dev/null
adb push "$HERE/out/libguestrt.so" "$tmp/libguest_rt.so" >/dev/null
for l in Cq fmodex fmodevent; do adb push "$DECOMP/lib/armeabi-v7a/lib$l.so" "$tmp/libguest_$l.so" >/dev/null; done
adb shell "chmod 644 $tmp/*"
adb shell "run-as $PKG sh -c 'mkdir -p files/dev && cp $tmp/* files/dev/'"
echo "pushed ($abi)"

if [ "${1:-}" = --run ]; then
    adb shell am force-stop $PKG
    adb logcat -c
    adb shell am start -n $PKG/.CqActivity >/dev/null
fi
