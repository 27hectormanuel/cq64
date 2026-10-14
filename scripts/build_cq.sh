#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# One-step Costume Quest builder: fetches the dependencies if needed, decompiles your APK and
# builds the 64-bit APK in the variant you choose.
#   scripts/build_cq.sh <your CostumeQuest.apk> [original|optimized] [--split] [--abi ABI]... [--icon FILE]
#
#   original   the game exactly as the 32-bit APK plays it (touch only, no developer menu), with
#              only the changes needed to run it on 64-bit-only phones
#   optimized  controller support with PC-style battle prompts and button icons, developer menu
#              (Lua console, FPS counter, frame cap, render resolution), full screen, Game Mode,
#              white menu highlight
#   --split    small APK + separate data pack (out/cqdata.zip), installed with
#              scripts/install_split.sh, for phones without room for the ~1.1 GB APK
#   --abi      arm64-v8a (phones) and/or x86_64 (the Android emulator); default: both
#   --icon     a higher-resolution square picture (.png/.jpg/.webp) for the launcher icon
# Without a variant, asks which one to build.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/env.sh"

APK= VARIANT= SPLIT=0 ABIS=()
while [ $# -gt 0 ]; do
    case "$1" in
    original | optimized) VARIANT=$1 ;;
    --split) SPLIT=1 ;;
    --abi) ABIS+=("${2:?--abi needs a value}"); shift ;;
    --icon) export ICON=${2:?--icon needs a file}; [ -f "$ICON" ] || { echo "no such file: $ICON" >&2; exit 1; }; shift ;;
    -h | --help) sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 1 ;;
    *) APK=$1 ;;
    esac
    shift
done
[ -n "$APK" ] || { echo "usage: $0 <your CostumeQuest.apk> [original|optimized] [--split] [--abi ABI]" >&2; exit 1; }
[ -f "$APK" ] || { echo "no such file: $APK" >&2; exit 1; }
if [ -z "$VARIANT" ]; then
    if [ -t 0 ]; then
        echo "Which build?"
        echo "  1) original   - 1:1 with the 32-bit APK (touch only, no developer menu)"
        echo "  2) optimized  - controller support, PC-style battle prompts, developer menu, Game Mode"
        read -rp "Choose 1 or 2 [2]: " choice
        case "${choice:-2}" in 1 | original) VARIANT=original ;; 2 | optimized) VARIANT=optimized ;;
            *) echo "invalid choice" >&2; exit 1 ;; esac
    else
        VARIANT=optimized
    fi
fi
[ ${#ABIS[@]} -eq 0 ] && ABIS=(arm64-v8a x86_64)

[ -f "$TP/apktool.jar" ] && [ -d "$TP/dynarmic" ] && [ -d "$TP/boost/boost" ] || "$HERE/fetch_deps.sh"
"$HERE/decompile.sh" "$APK"
if [ "$SPLIT" = 1 ]; then
    VARIANT=$VARIANT SPLIT=1 "$HERE/build_apk.sh" "${ABIS[@]}"
    name=costumequest64.apk; [ "$VARIANT" = original ] && name=costumequest64-original.apk
    echo
    echo "Built out/$name and out/cqdata.zip. Install with:"
    echo "  VARIANT=$VARIANT scripts/install_split.sh out"
else
    VARIANT=$VARIANT FULL=1 "$HERE/build_apk.sh" "${ABIS[@]}"
    name=costumequest64.apk; [ "$VARIANT" = original ] && name=costumequest64-original.apk
    echo
    echo "Built out/$name. Install with:"
    echo "  adb install -r --no-incremental out/$name"
fi
