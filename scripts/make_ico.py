#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# Makes a Windows .ico (256, 128, 64, 48, 32 and 16 px, PNG-compressed) from a square picture,
# scaled with ffmpeg. Usage: make_ico.py <picture> <out.ico>
import os
import struct
import subprocess
import sys
import tempfile

src, out = sys.argv[1], sys.argv[2]
sizes = [256, 128, 64, 48, 32, 16]
images = []
with tempfile.TemporaryDirectory() as tmp:
    for s in sizes:
        png = os.path.join(tmp, f"{s}.png")
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", src, "-vf",
                        f"scale={s}:{s}:flags=lanczos,format=rgba", png], check=True)
        images.append(open(png, "rb").read())
offset = 6 + 16 * len(sizes)
header = struct.pack("<HHH", 0, 1, len(sizes))
entries = b""
for s, data in zip(sizes, images):
    entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
with open(out, "wb") as f:
    f.write(header + entries + b"".join(images))
