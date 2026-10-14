// SPDX-License-Identifier: CC0-1.0
#pragma once
// Same-size byte patches to files inside the game's data pack, applied as the game reads them
// (the pack itself is never modified). See datapatch.cpp for the patch list.
#include "common.h"

#include <string>
#include <utility>
#include <vector>

namespace datapatch {
// Locates the patched files in the pack (the APK or cqdata.zip, holding assets/ant.mp3).
void init(const std::string &pack_path);
// Patches `buf`, just read from host fd `fd` at file offset `pos`, if it covers a patch.
void apply(int fd, s64 pos, void *buf, s64 n);
// The data pack member replaced by the generated button icon library (mobile + Xbox icons).
constexpr const char *ICON_SLOT = "Data/UI/Mobile_InputIcons/Opt/Mobile_InputIcons.gfx";
// Whether that library was built; its Xbox images then need their textures mapped (gamepad.cpp).
bool icon_library_ready();
// The game's Lua scripts (path, source) read from the pack given to init().
std::vector<std::pair<std::string, std::string>> lua_scripts();
}  // namespace datapatch
