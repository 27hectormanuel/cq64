#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Starts Costume Quest Mobile.exe in Steam's Proton on Linux (e.g. a Steam Deck), for testing.
#   ./play-on-linux.sh [your Costume Quest APK]
# The first time, give it your APK (or drop CostumeQuest.apk next to this script); it is remembered.
#   PROTON=<Proton directory>   use that Proton (default: the newest one Steam has installed)
#   CQ_PREFIX=<directory>       Wine prefix to use (default: proton-prefix/ next to this script)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
STEAM=${STEAM_ROOT:-$HOME/.steam/steam}
[ -d "$STEAM/steamapps" ] || STEAM=$HOME/.local/share/Steam
[ -d "$STEAM/steamapps" ] || { echo "Steam not found (set STEAM_ROOT)" >&2; exit 1; }

if [ -z "${PROTON:-}" ]; then
    # Steam libraries (libraryfolders.vdf) and custom Proton builds (compatibilitytools.d).
    libs=("$STEAM")
    while IFS= read -r p; do libs+=("$p"); done < <(grep -oP '"path"\s+"\K[^"]+' "$STEAM/steamapps/libraryfolders.vdf" 2>/dev/null || true)
    candidates=()
    for l in "${libs[@]}"; do
        for d in "$l"/steamapps/common/Proton*/; do [ -x "$d/proton" ] && candidates+=("${d%/}"); done
    done
    for d in "$STEAM"/compatibilitytools.d/*/; do [ -x "$d/proton" ] && candidates+=("${d%/}"); done
    [ ${#candidates[@]} -gt 0 ] || { echo "No Proton found: install Proton in Steam (or set PROTON=...)" >&2; exit 1; }
    # Numbered releases first, newest last; Experimental only if nothing else is installed.
    PROTON=$(printf '%s\n' "${candidates[@]}" | grep -v Experimental | sort -V | tail -1)
    [ -n "$PROTON" ] || PROTON=${candidates[0]}
fi
echo "Using $(basename "$PROTON")"

export STEAM_COMPAT_CLIENT_INSTALL_PATH=$STEAM
export STEAM_COMPAT_DATA_PATH=${CQ_PREFIX:-$HERE/proton-prefix}
mkdir -p "$STEAM_COMPAT_DATA_PATH"

args=()
if [ $# -gt 0 ]; then
    apk=$(realpath "$1")
    [ -f "$apk" ] || { echo "No such file: $1" >&2; exit 1; }
    args+=("Z:${apk//\//\\}")  # Wine sees the Linux root as drive Z:
fi
cd "$HERE"
exec "$PROTON/proton" run "$HERE/Costume Quest Mobile.exe" "${args[@]}"
