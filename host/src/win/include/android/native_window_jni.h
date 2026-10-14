// SPDX-License-Identifier: CC0-1.0
#pragma once
#include <android/native_window.h>
#include <jni.h>
#ifdef __cplusplus
extern "C" {
#endif
// The stand-in JVM's Surface object -> the game window (fakejvm.cpp).
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject surface);
#ifdef __cplusplus
}
#endif
