#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Install the slim APK and stream the data pack into the app's private storage (no temporary
# copy on the phone, so this needs ~1.1 GB free instead of the full APK's ~2.5 GB).
#   ./install_split.sh <dir containing costumequest64.apk and cqdata.zip>
set -euo pipefail
DIR=${1:?usage: $0 <dir>}
PKG=com.doublefine.cq
adb install -r --no-incremental "$DIR/costumequest64.apk"
adb shell "run-as $PKG mkdir -p files"
adb exec-in "run-as $PKG sh -c 'cat > files/cqdata.zip'" < "$DIR/cqdata.zip"
# exec-in can return before the phone has flushed the last bytes; wait for the full size.
want=$(stat -c %s "$DIR/cqdata.zip")
for i in $(seq 1 30); do
    have=$(adb shell "run-as $PKG stat -c %s files/cqdata.zip" | tr -d '\r')
    [ "$have" = "$want" ] && { echo "data pack installed ($have bytes)"; exit 0; }
    sleep 1
done
echo "data pack incomplete: $have of $want bytes" >&2
exit 1
