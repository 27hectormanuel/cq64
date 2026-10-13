#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Downloads the pinned third-party sources into third_party/ (not part of this repository):
#   dynarmic  ARM32 JIT (azahar-emu fork, 0BSD) and its submodules
#   Boost     headers used by dynarmic (BSL-1.0)
#   apktool   APK (de)compiler (Apache-2.0)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP=$ROOT/third_party

DYNARMIC_URL=https://github.com/azahar-emu/dynarmic.git
DYNARMIC_REV=a46601580d5512d324104f985b5f0209dc980ddc
BOOST_URL=https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2
BOOST_SHA256=1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b
APKTOOL_URL=https://github.com/iBotPeaches/Apktool/releases/download/v2.11.1/apktool_2.11.1.jar
APKTOOL_SHA256=56d59c524fc764263ba8d345754d8daf55b1887818b15cd3b594f555d249e2db

check() { echo "$2  $1" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; exit 1; }; }

if [ ! -d "$TP/dynarmic/.git" ]; then
    echo "== dynarmic"
    git clone --quiet "$DYNARMIC_URL" "$TP/dynarmic"
    git -C "$TP/dynarmic" checkout --quiet "$DYNARMIC_REV"
    git -C "$TP/dynarmic" submodule update --quiet --init --recursive --depth 1
fi

if [ ! -d "$TP/boost/boost" ]; then
    echo "== Boost headers"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    curl -fL --progress-bar -o "$tmp/boost.tar.bz2" "$BOOST_URL"
    check "$tmp/boost.tar.bz2" "$BOOST_SHA256"
    mkdir -p "$TP/boost"
    tar -xjf "$tmp/boost.tar.bz2" -C "$TP/boost" --strip-components=1 boost_1_86_0/boost boost_1_86_0/LICENSE_1_0.txt
fi

if [ ! -f "$TP/apktool.jar" ]; then
    echo "== apktool"
    curl -fL --progress-bar -o "$TP/apktool.jar.part" "$APKTOOL_URL"
    check "$TP/apktool.jar.part" "$APKTOOL_SHA256"
    mv "$TP/apktool.jar.part" "$TP/apktool.jar"
fi
echo "third-party sources ready in $TP"
