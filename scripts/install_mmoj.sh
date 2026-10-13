#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Installs the Middle Manager of Justice build and copies its OBB data file to the phone's shared
# storage (Android/obb/com.doublefine.mmoj/), where the game looks for it.
#   scripts/install_mmoj.sh [out/mmoj64.apk]
set -euo pipefail
source "$(dirname "$0")/env.sh"
APK=${1:-$ROOT/out/mmoj64.apk}
OBB=$MMOJ_WORK/main.121340.com.doublefine.mmoj.obb
[ -f "$OBB" ] || { echo "no data file at $OBB: run scripts/decompile_mmoj.sh first" >&2; exit 1; }
adb install -r --no-incremental "$APK"
adb shell mkdir -p /sdcard/Android/obb/com.doublefine.mmoj
adb push "$OBB" /sdcard/Android/obb/com.doublefine.mmoj/
echo "installed; the data file is in /sdcard/Android/obb/com.doublefine.mmoj/"
