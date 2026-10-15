#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Windows build: cq64.exe runs Costume Quest's Android version on Windows without an Android
# emulator (the same ARM translator and host as the Android build, SDL2 for the window, input and
# sound, ANGLE for OpenGL ES on Direct3D). Cross-compiled from Linux or WSL2; no APK needed to
# build: the program reads everything from the player's APK when it starts.
#   scripts/build_win.sh [optimized|original] [--icon <picture>]
#   --icon   a square picture (.png/.jpg, e.g. the game's 1024 px store icon) for the program's icon;
#            also ICON=<picture>. Not part of this repository: bring your own.
# Output (OUT_DIR, default out/): windows/ ("Costume Quest Mobile.exe", its DLLs, play-on-linux.sh for Proton) and
# CostumeQuestMobile-<variant>.zip.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/env.sh"
VARIANT=optimized
ICON=${ICON:-}
while [ $# -gt 0 ]; do
    case "$1" in
    optimized | original) VARIANT=$1 ;;
    --icon) ICON=$2; shift ;;
    *) echo "usage: scripts/build_win.sh [optimized|original] [--icon <picture>]" >&2; exit 1 ;;
    esac
    shift
done
D=${WIN_DEPS:-$TP/win}
OUT_DIR=${OUT_DIR:-$ROOT/out}  # where windows/ and the zip go
OUT=$OUT_DIR/windows
BUILD=${WIN_BUILD:-$ROOT/build/win-$VARIANT}

"$HERE/fetch_deps.sh"
"$HERE/fetch_win_deps.sh"
NDK=$NDK LLVM_MINGW="$D/llvm-mingw" "$ROOT/guest/build.sh" "$BUILD/guest" >/dev/null

WIN_ICON=
if [ -n "$ICON" ]; then
    mkdir -p "$BUILD"
    WIN_ICON=$BUILD/cq64.ico
    python3 "$HERE/make_ico.py" "$ICON" "$WIN_ICON"
fi
cmake -S "$ROOT/host" -B "$BUILD" -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/host/src/win/toolchain-mingw.cmake" -DLLVM_MINGW="$D/llvm-mingw" \
    -DWIN_DEPS="$D" -DGUEST_RT="$BUILD/guest/libguestrt.so" -DWIN_ICON="$WIN_ICON" -DGAME=cq -DCQ_VARIANT="$VARIANT" >/dev/null
cmake --build "$BUILD" --target cq64

rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BUILD/cq64.exe" "$OUT/Costume Quest Mobile.exe"
cp "$ROOT/host/src/win/play-on-linux.sh" "$OUT/"
cp "$D/sdl2/ucrt64/bin/SDL2.dll" "$D/angle/"{libEGL.dll,libGLESv2.dll,d3dcompiler_47.dll} \
    "$D/llvm-mingw/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$OUT/"
cp "$D/angle/LICENSES.chromium.html" "$OUT/ANGLE-LICENSES.html"
cp "$D/sdl2/ucrt64/share/licenses/SDL2/LICENSE.txt" "$OUT/SDL2-LICENSE.txt"
cat > "$OUT/README.txt" <<TXT
Costume Quest Mobile for Windows ($VARIANT build) - CQ64, https://github.com/27hectormanuel/cq64

Costume Quest's Android version on Windows, without an Android emulator. You need your own copy of
Costume Quest for Android (an APK, v1.1.7); nothing of the game is included here.

  Drag your Costume Quest APK onto "Costume Quest Mobile.exe".

It remembers the APK (cq64-data/apk.txt), so afterwards just start it. Started without one, it uses
CostumeQuest.apk next to it or asks for the file. Saves and settings go to cq64-data/, the log to
cq64.log; F12 saves a screenshot (screenshot.bmp).

Linux / Steam Deck: ./play-on-linux.sh /path/to/CostumeQuest.apk  (runs it in Steam's Proton)

Controls: mouse = touch; arrow keys, Enter, Escape (back); game controllers work like on Android.

Includes SDL2 (zlib license), ANGLE from Chromium/Electron (BSD; ANGLE-LICENSES.html) and the
MinGW-w64 winpthreads runtime. CQ64 itself is public domain (CC0).
TXT
ZIP=$OUT_DIR/CostumeQuestMobile-$VARIANT.zip
rm -f "$ZIP"
(cd "$OUT" && zip -qr "$ZIP" .)
echo "Built $OUT and $ZIP: drag your Costume Quest APK onto \"Costume Quest Mobile.exe\"."
