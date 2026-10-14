#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Windows build (proof of concept): cq64.exe runs Costume Quest's Android code on Windows without
# an Android emulator: the same ARM translator and host as the Android build, with SDL2 for the
# window and input and ANGLE for OpenGL ES on Direct3D. Cross-compiled from Linux or WSL2.
#   scripts/build_win.sh <your CostumeQuest.apk> [original|optimized]
# Output: out/windows/ with cq64.exe, its DLLs and the game's ARM libraries from your APK. Put
# your APK next to cq64.exe as CostumeQuest.apk (or pass its path to cq64.exe) and run it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/env.sh"
APK=${1:?usage: scripts/build_win.sh <your CostumeQuest.apk> [original|optimized]}
VARIANT=${2:-optimized}
D=${WIN_DEPS:-$TP/win}
OUT=$ROOT/out/windows
BUILD=${WIN_BUILD:-$ROOT/build/win-$VARIANT}

"$HERE/fetch_deps.sh"
"$HERE/fetch_win_deps.sh"
NDK=$NDK "$ROOT/guest/build.sh" "$BUILD/guest" >/dev/null

cmake -S "$ROOT/host" -B "$BUILD" -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/host/src/win/toolchain-mingw.cmake" -DLLVM_MINGW="$D/llvm-mingw" \
    -DWIN_DEPS="$D" -DGAME=cq -DCQ_VARIANT="$VARIANT" >/dev/null
cmake --build "$BUILD" --target cq64

rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BUILD/cq64.exe" "$D/sdl2/ucrt64/bin/SDL2.dll" "$D/angle/"{libEGL.dll,libGLESv2.dll,d3dcompiler_47.dll} \
    "$D/llvm-mingw/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$OUT/"
cp "$D/angle/LICENSES.chromium.html" "$OUT/ANGLE-LICENSES.html"
cp "$D/sdl2/ucrt64/share/licenses/SDL2/LICENSE.txt" "$OUT/SDL2-LICENSE.txt"
cp "$BUILD/guest/libguestrt.so" "$OUT/libguest_rt.so"
for l in Cq fmodex fmodevent; do
    unzip -p "$APK" "lib/armeabi-v7a/lib$l.so" > "$OUT/libguest_$l.so"
done
echo "Built $OUT/cq64.exe. Copy your APK there as CostumeQuest.apk (or pass its path) and run cq64.exe."
