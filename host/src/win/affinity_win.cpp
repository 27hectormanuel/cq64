// SPDX-License-Identifier: CC0-1.0
// Windows build: no core placement or Android performance hints; a desktop CPU has headroom.
#include "../affinity.h"

namespace affinity {
void start(const std::string &) {}
void frame_presented() {}
}  // namespace affinity
