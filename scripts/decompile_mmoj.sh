#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Prepares your copy of Middle Manager of Justice (v1.3.4, versionCode 121340) for build_mmoj.sh:
# decompiles the APK into work/mmoj/decompiled and keeps the OBB data file next to it.
#   scripts/decompile_mmoj.sh MMoJ.xapk                 (XAPK bundle: APK + OBB)
#   scripts/decompile_mmoj.sh com.doublefine.mmoj.apk main.121340.com.doublefine.mmoj.obb
set -euo pipefail
source "$(dirname "$0")/env.sh"
IN=${1:?usage: $0 <MMoJ .xapk> | <MMoJ .apk> <main .obb>}
[ -f "$TP/apktool.jar" ] || { echo "run scripts/fetch_deps.sh first" >&2; exit 1; }
OBB_NAME=main.121340.com.doublefine.mmoj.obb
rm -rf "$MMOJ_WORK"
mkdir -p "$MMOJ_WORK"
case "$IN" in
*.xapk)
    unzip -q -o "$IN" com.doublefine.mmoj.apk "Android/obb/com.doublefine.mmoj/*" -d "$MMOJ_WORK/xapk"
    APK=$MMOJ_WORK/xapk/com.doublefine.mmoj.apk
    OBB=$(ls "$MMOJ_WORK"/xapk/Android/obb/com.doublefine.mmoj/main.*.obb | head -1)
    ;;
*)
    APK=$IN
    OBB=${2:?the OBB data file is needed too}
    ;;
esac
java -jar "$TP/apktool.jar" d -f "$APK" -o "$DECOMP_MMOJ"
mv -f "$OBB" "$MMOJ_WORK/$OBB_NAME" 2>/dev/null || cp "$OBB" "$MMOJ_WORK/$OBB_NAME"
rm -rf "$MMOJ_WORK/xapk"
version=$(sed -n 's/^ *versionName: *//p' "$DECOMP_MMOJ/apktool.yml" | tr -d "'")
[ "$version" = 1.3.4 ] || echo "warning: tested with version 1.3.4, this APK is $version" >&2
echo "decompiled to $DECOMP_MMOJ; data file at $MMOJ_WORK/$OBB_NAME"
