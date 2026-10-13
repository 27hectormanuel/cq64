// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Same-size byte patches to files inside the game's data pack, applied as the game reads them
// (the pack itself is never modified). See datapatch.cpp for the patch list.
#include "common.h"

namespace datapatch {
// Locates the patched files in the pack (the APK or cqdata.zip, holding assets/ant.mp3).
void init(const std::string &pack_path);
// Patches `buf`, just read from host fd `fd` at file offset `pos`, if it covers a patch.
void apply(int fd, s64 pos, void *buf, s64 n);
}  // namespace datapatch
