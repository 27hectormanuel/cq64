// SPDX-License-Identifier: CC0-1.0
// Argument hints for the debug console's command list, worked out at run time from the game's
// own code and scripts (see luahints.cpp).
#pragma once

#include <string>

namespace luahints {
// One line per hint, tab-separated:
//   C <name> <min args> <max args> <type,...>   engine (C) function, by its registered name
//   S <table.name> <parameter list>             function defined in a script
//   E <table.name> <arguments>                  a call with literal arguments found in a script
// Built on first use; empty if the game has no Lua function registry (other engines).
const std::string &get();
}  // namespace luahints
