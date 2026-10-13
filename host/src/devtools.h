// SPDX-License-Identifier: CC0-1.0
#pragma once
// Developer tools: the revived debug command line, fps readout and frame-rate cap.
#include <jni.h>
#include <string>

namespace devtools {
// Queues a Lua console command; it runs on the game thread at the next frame.
void submit(const std::string &cmd);
// Called from eglSwapBuffers on the render thread, before the swap; paces to the frame cap.
void frame_presented();
// Registers com.doublefine.cq.DevTools natives, if that class is present.
void register_natives(JNIEnv *env);
}  // namespace devtools
