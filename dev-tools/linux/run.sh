#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# run.sh <game dir> [game options]: starts the official Linux Costume Quest (the folder holding
# Cq.bin.x86) with the debug console. ` or F1 opens it. The Steam runtime's 32-bit libraries are
# used when Steam is installed (the game needs libGLU, which few systems ship for i386).
#   CQCONSOLE_DIR=<dir>   where the console's log and test files go (default: the game dir)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
GAME=$(cd "${1:?usage: run.sh <game dir> [options]}" && pwd); shift
RT=${STEAM_RUNTIME_DIR:-$HOME/.steam/steam/ubuntu12_32/steam-runtime}
cd "$GAME"
export LD_LIBRARY_PATH="$GAME/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Preloaded into the (32-bit) game only, not the runtime's 64-bit helpers.
PRE="LD_PRELOAD=$HERE/cqconsole.so${LD_PRELOAD:+:$LD_PRELOAD}"
if [ -x "$RT/run.sh" ]; then exec "$RT/run.sh" env "$PRE" ./Cq.bin.x86 "$@"; fi
exec env "$PRE" ./Cq.bin.x86 "$@"
