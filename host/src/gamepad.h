// SPDX-License-Identifier: CC0-1.0
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
// Debug: plays battle prompts automatically with synthetic controller input while on.
void set_auto(bool on);
// Debug: makes every battle prompt this BattlePrompt (-1: the game's own).
void force_prompt(int prompt);
// Registers com.doublefine.cq.DevTools.nativePad, if that class is present.
void register_natives(JNIEnv *env);
}  // namespace gamepad
