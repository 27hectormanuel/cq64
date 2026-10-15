// SPDX-License-Identifier: CC0-1.0
#pragma once
// Windows build: the in-game debug command line (` or F1). See console_win.cpp.
#include <SDL2/SDL.h>
#include <string>

namespace winconsole {
// Main thread: takes the event if the console uses it (then the game must not see it).
bool handle_event(const SDL_Event &e);
bool is_open();
// GL thread, before the swap: draws the console over the frame when it is open.
void draw(int width, int height);
}  // namespace winconsole
