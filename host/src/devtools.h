// SPDX-License-Identifier: CC0-1.0
#pragma once
// Developer tools: the revived debug command line, fps readout and frame-rate cap.
#include <jni.h>
#include <string>

namespace devtools {
// Queues a Lua console command; it runs on the game thread at the next frame.
void submit(const std::string &cmd);
// The recent console output (last 16 KB), oldest first; clear_output() empties it.
std::string output_text();
void clear_output();
// Text passed to the console's hostlist() (the command list), as the Android dev menu reads it.
std::string command_list();
// Called from eglSwapBuffers on the render thread, before the swap; paces to the frame cap.
void frame_presented();
// Registers com.doublefine.cq.DevTools natives, if that class is present.
void register_natives(JNIEnv *env);
}  // namespace devtools
