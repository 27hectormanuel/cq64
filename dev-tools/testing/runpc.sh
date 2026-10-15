#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# runpc.sh [game options]: starts the official Cq.exe under Proton with the console DLL, from a
# folder of symlinks to the game (so the game folder itself stays untouched).
#   CQ_GAME=<folder with Cq.exe>  CQ_DLL=<dinput8.dll>  CQ_RUN=<scratch folder>  CQ_PREFIX=<Wine prefix>
#   PROTON=<Proton folder>        (default: the newest numbered Proton Steam has)
set -euo pipefail
: "${CQ_GAME:?set CQ_GAME}" "${CQ_DLL:?set CQ_DLL}"
RUN=${CQ_RUN:-/tmp/cqpc}; PFX=${CQ_PREFIX:-$RUN-prefix}
STEAM=${STEAM_ROOT:-$HOME/.steam/steam}
PROTON=${PROTON:-$(ls -d "$STEAM"/steamapps/common/Proton\ [0-9]* | sort -V | tail -1)}
mkdir -p "$RUN" "$PFX"
for f in "$CQ_GAME"/*; do ln -sfn "$f" "$RUN/$(basename "$f")"; done
cp "$CQ_DLL" "$RUN/dinput8.dll"
export STEAM_COMPAT_CLIENT_INSTALL_PATH=$STEAM STEAM_COMPAT_DATA_PATH=$PFX WINEDLLOVERRIDES=dinput8=n,b
cd "$RUN"
exec "$PROTON/proton" run ./Cq.exe "$@"
