// SPDX-License-Identifier: CC0-1.0
#pragma once
#include <string>

// Rewrites guest file paths. Old Android storage locations (/sdcard, ...) that a modern
// app cannot access are redirected into the app's external files directory.
namespace paths {
void add_mapping(const std::string &from_prefix, const std::string &to_prefix);
std::string translate(const char *guest_path);
}  // namespace paths
