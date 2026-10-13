#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Decompiles your copy of the Costume Quest Android APK (v1.1.7, versionCode 121030) into
# work/decompiled, the input of build_apk.sh.
#   scripts/decompile.sh CostumeQuest.apk
set -euo pipefail
source "$(dirname "$0")/env.sh"
APK=${1:?usage: $0 <Costume Quest APK>}
[ -f "$TP/apktool.jar" ] || { echo "run scripts/fetch_deps.sh first" >&2; exit 1; }
rm -rf "$DECOMP"
mkdir -p "$(dirname "$DECOMP")"
java -jar "$TP/apktool.jar" d -f "$APK" -o "$DECOMP"
for f in lib/armeabi-v7a/libCq.so lib/armeabi-v7a/libfmodex.so lib/armeabi-v7a/libfmodevent.so assets/ant.mp3; do
    [ -f "$DECOMP/$f" ] || { echo "warning: $f missing from the APK" >&2; }
done
version=$(sed -n 's/^ *versionName: *//p' "$DECOMP/apktool.yml" | tr -d "'")
[ "$version" = 1.1.7 ] || echo "warning: tested with version 1.1.7, this APK is $version" >&2
echo "decompiled to $DECOMP"
