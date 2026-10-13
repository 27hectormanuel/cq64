// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Gamepad support: libCq's SDL only exposes the accelerometer as a joystick, so its Android
// joystick backend is replaced with a virtual pad that the Java side (DevTools) feeds from real
// controllers. The engine picks it up as an SDL game controller (see gamepad.cpp).
#include "common.h"

#include <jni.h>

struct Module;

namespace gamepad {
// Patches libCq's SDL joystick backend; call after the guest libraries are loaded, before the
// game initializes SDL.
void install(Module *cq);
// Holds virtual pad buttons `mask` (bit i = button i, see gamepad.cpp) for `ms` milliseconds.
void debug_press(u32 mask, int ms);
// Registers com.doublefine.cq.DevTools.nativePad, if that class is present.
void register_natives(JNIEnv *env);
}  // namespace gamepad
