#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Downloads what the Windows build needs (not part of this repository), checksum-verified, into
# ${WIN_DEPS:-third_party/win}:
#   llvm-mingw  clang cross-compiler for Windows (Apache-2.0 with LLVM exceptions; MinGW-w64 runtime)
#   SDL2        window, input and audio (zlib license), MSYS2 UCRT64 package
#   zlib        (zlib license), MSYS2 UCRT64 package
#   ANGLE       OpenGL ES on Direct3D (BSD-3-Clause), libEGL.dll/libGLESv2.dll from Electron's
#               official Windows release (Chromium's own build of ANGLE)
# plus the Khronos EGL/GLES headers and jni.h from the Android NDK, and import libraries for ANGLE.
set -euo pipefail
. "$(dirname "$0")/env.sh"
D=${WIN_DEPS:-$TP/win}
mkdir -p "$D/dl"

LLVM_MINGW=llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64
LLVM_MINGW_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/$LLVM_MINGW.tar.xz
LLVM_MINGW_SHA256=5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4
SDL2_URL=https://repo.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-SDL2-2.32.8-1-any.pkg.tar.zst
SDL2_SHA256=d8604dcb9a666f1e4421375d41b1294a21fe46bda3affaed4b9c212974867d9a
ZLIB_URL=https://repo.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-zlib-1.3.2-2-any.pkg.tar.zst
ZLIB_SHA256=841401182976d2f9e17e5c0ebaac51f2a8014140ea53d67625e91c8fb3c85ea0
ELECTRON=v43.7.11  # the last line of Electron releases that ships ANGLE as separate DLLs
ELECTRON_URL=https://github.com/electron/electron/releases/download/$ELECTRON/electron-$ELECTRON-win32-x64.zip
ELECTRON_SHA256=4948fb44c6d466b361734f43252846286728adb0bd2f348a95c7fbf5fae21122

check() { echo "$2  $1" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; exit 1; }; }
get() {  # url sha256 -> downloaded file path
    local f="$D/dl/$(basename "$1")"
    [ -f "$f" ] || curl -fL --progress-bar -o "$f.part" "$1" && { [ -f "$f" ] || mv "$f.part" "$f"; }
    check "$f" "$2"
    echo "$f"
}

if [ ! -x "$D/llvm-mingw/bin/x86_64-w64-mingw32-clang++" ]; then
    echo "== llvm-mingw"
    f=$(get "$LLVM_MINGW_URL" "$LLVM_MINGW_SHA256")
    tar -xJf "$f" -C "$D" && rm -rf "$D/llvm-mingw" && mv "$D/$LLVM_MINGW" "$D/llvm-mingw" && rm -f "$f"
fi
if [ ! -f "$D/sdl2/ucrt64/bin/SDL2.dll" ]; then
    echo "== SDL2"
    f=$(get "$SDL2_URL" "$SDL2_SHA256")
    mkdir -p "$D/sdl2" && tar --zstd -xf "$f" -C "$D/sdl2"
fi
if [ ! -f "$D/zlib/ucrt64/lib/libz.a" ]; then
    echo "== zlib"
    f=$(get "$ZLIB_URL" "$ZLIB_SHA256")
    mkdir -p "$D/zlib" && tar --zstd -xf "$f" -C "$D/zlib"
fi
if [ ! -f "$D/angle/libGLESv2.dll" ]; then
    echo "== ANGLE (from Electron $ELECTRON)"
    f=$(get "$ELECTRON_URL" "$ELECTRON_SHA256")
    mkdir -p "$D/angle" && unzip -qjo "$f" libEGL.dll libGLESv2.dll d3dcompiler_47.dll LICENSES.chromium.html -d "$D/angle"
    rm -f "$f"
fi
if [ ! -f "$D/lib/libGLESv2.dll.a" ]; then
    echo "== ANGLE import libraries"
    mkdir -p "$D/lib"
    for dll in libEGL libGLESv2; do
        { echo "LIBRARY $dll.dll"; echo EXPORTS
          "$D/llvm-mingw/bin/llvm-readobj" --coff-exports "$D/angle/$dll.dll" | awk '/Name: /{print $2}'; } > "$D/lib/$dll.def"
        "$D/llvm-mingw/bin/llvm-dlltool" -m i386:x86-64 -d "$D/lib/$dll.def" -l "$D/lib/$dll.dll.a"
    done
fi
if [ ! -f "$D/khronos/jni.h" ]; then
    echo "== Khronos and JNI headers (from the NDK)"
    inc=$NDK_TC/sysroot/usr/include
    mkdir -p "$D/khronos" && cp -r "$inc"/{EGL,GLES,GLES2,GLES3,KHR} "$inc/jni.h" "$D/khronos/"
fi
echo "Windows dependencies ready in $D"
