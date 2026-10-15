#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# pcshot.sh <out.jpg>: captures the official Windows game's frame (needs the console DLL).
#   CQ_RUN=<the folder runpc.sh runs the game from>   (default /tmp/cqpc)
R=${CQ_RUN:-/tmp/cqpc}
rm -f "$R/cqconsole-shot.bmp"; touch "$R/cqconsole-shot"
for i in $(seq 1 30); do [ -f "$R/cqconsole-shot.bmp" ] && break; sleep 0.2; done; sleep 0.5
ffmpeg -loglevel error -y -i "$R/cqconsole-shot.bmp" -vf scale=640:-1 "$1"
