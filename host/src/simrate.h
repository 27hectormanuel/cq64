// SPDX-License-Identifier: CC0-1.0
// High frame rate: Costume Quest's simulation at 60 (or 120) Hz instead of 30 (simrate.cpp).
#pragma once

#include "loader.h"

namespace simrate {
// The rate set in files/simrate: 30 (the game's own), 60 or 120.
int configured_rate();
// Applies a configured rate above 30 Hz; call once the game library is loaded, before it runs.
void install(Module *cq);
}  // namespace simrate
