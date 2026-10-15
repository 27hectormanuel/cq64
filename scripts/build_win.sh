#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Windows build: cq64.exe runs Costume Quest's Android version on Windows without an Android
# emulator (the same ARM translator and host as the Android build, SDL2 for the window, input and
# sound, ANGLE for OpenGL ES on Direct3D). Cross-compiled from Linux or WSL2; no APK needed to
# build: the program reads everything from the player's APK when it starts.
#   scripts/build_win.sh [optimized|original]
# Output: out/windows/ (cq64.exe and its DLLs) and out/cq64-windows-<variant>.zip.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/env.sh"
VARIANT=${1:-optimized}
D=${WIN_DEPS:-$TP/win}
OUT=$ROOT/out/windows
BUILD=${WIN_BUILD:-$ROOT/build/win-$VARIANT}

"$HERE/fetch_deps.sh"
"$HERE/fetch_win_deps.sh"
NDK=$NDK "$ROOT/guest/build.sh" "$BUILD/guest" >/dev/null

cmake -S "$ROOT/host" -B "$BUILD" -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/host/src/win/toolchain-mingw.cmake" -DLLVM_MINGW="$D/llvm-mingw" \
    -DWIN_DEPS="$D" -DGUEST_RT="$BUILD/guest/libguestrt.so" -DGAME=cq -DCQ_VARIANT="$VARIANT" >/dev/null
cmake --build "$BUILD" --target cq64

rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BUILD/cq64.exe" "$D/sdl2/ucrt64/bin/SDL2.dll" "$D/angle/"{libEGL.dll,libGLESv2.dll,d3dcompiler_47.dll} \
    "$D/llvm-mingw/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$OUT/"
cp "$D/angle/LICENSES.chromium.html" "$OUT/ANGLE-LICENSES.html"
cp "$D/sdl2/ucrt64/share/licenses/SDL2/LICENSE.txt" "$OUT/SDL2-LICENSE.txt"
cat > "$OUT/README.txt" <<TXT
CQ64 for Windows ($VARIANT build) - https://github.com/27hectormanuel/cq64

Costume Quest's Android version on Windows, without an Android emulator. You need your own copy of
Costume Quest for Android (the APK, v1.1.7); nothing of the game is included here.

  Drag your Costume Quest APK onto cq64.exe.

cq64.exe remembers it (cq64-data/apk.txt), so afterwards just start cq64.exe. Started without one,
it uses CostumeQuest.apk next to it or asks for the file. Saves and settings go to cq64-data/,
the log to cq64.log; F12 saves a screenshot (screenshot.bmp).

Controls: mouse = touch; arrow keys, Enter, Escape (back); game controllers work like on Android.

Includes SDL2 (zlib license), ANGLE from Chromium/Electron (BSD; ANGLE-LICENSES.html) and the
MinGW-w64 winpthreads runtime. CQ64 itself is public domain (CC0).
TXT
rm -f "$ROOT/out/cq64-windows-$VARIANT.zip"
(cd "$OUT" && zip -qr "$ROOT/out/cq64-windows-$VARIANT.zip" .)
echo "Built $OUT and out/cq64-windows-$VARIANT.zip: drag your Costume Quest APK onto cq64.exe."
