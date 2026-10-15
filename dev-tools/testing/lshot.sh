#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# lshot.sh <out.jpg>: captures the official Linux game's frame (needs the console library).
#   CQCONSOLE_DIR=<the console's file folder>   (as given to run.sh; default: current dir)
R=${CQCONSOLE_DIR:-.}
rm -f "$R/cqconsole-shot.bmp"; touch "$R/cqconsole-shot"
for i in $(seq 1 30); do [ -f "$R/cqconsole-shot.bmp" ] && break; sleep 0.2; done; sleep 0.5
ffmpeg -loglevel error -y -i "$R/cqconsole-shot.bmp" -vf scale=640:-1 "$1"
