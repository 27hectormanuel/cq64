// SPDX-License-Identifier: CC0-1.0
// Keeps the game's busiest thread on the fastest CPU core (affinity.cpp).
#pragma once

#include <string>

namespace affinity {
// Starts the monitor thread if `flag_file` exists.
void start(const std::string &flag_file);
// Call once per presented frame: reports the frame time to the performance hint session.
void frame_presented();
}  // namespace affinity
