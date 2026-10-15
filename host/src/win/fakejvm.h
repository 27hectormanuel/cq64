// SPDX-License-Identifier: CC0-1.0
#pragma once
// Windows build: a stand-in for the Java VM. On Android the game's Java side (SDLActivity,
// CqActivity, FMODAudioDevice) creates the window, starts the game and answers its JNI calls; here
// a small object model with the few Java classes and methods the game uses plays that part. Methods
// nobody implemented are logged and return zero, so new needs show up in cq64.log.
#include <jni.h>

#include <string>

namespace win {
// Host paths: the game's private files directory and the user's APK (set by win_main).
std::string files_dir();
std::string apk_path();
// Called on the game's render thread before each frame is presented (screenshots).
void before_swap();
// The path the game sees for its APK ("/data/app/.../base.apk"), mapped to apk_path().
extern const char *const kGuestApk;
}  // namespace win

namespace fakejvm {
void init(void *hwnd, int width, int height);
JavaVM *vm();
JNIEnv *env();

jobject activity();
jobject asset_manager();
jobject surface();
jstring new_string(const char *utf8);
jobject new_object(const char *cls);
jobject new_direct_buffer(void *addr, jlong capacity);
void set_window_size(int width, int height);

// A native method registered with RegisterNatives (by JNI_OnLoad), or null.
void *native(const char *cls, const char *name);
jclass find_class(const char *name);

// Calls a registered native the way Java would: static natives get their class as `this`.
template <class R = void, class... A> R call_static(const char *cls, const char *name, A... a) {
    using Fn = R (*)(JNIEnv *, jclass, A...);
    Fn fn = (Fn)native(cls, name);
    if (!fn) return R();
    return fn(env(), find_class(cls), a...);
}

// Calls a registered instance native on `self`.
template <class R = void, class... A> R call(jobject self, const char *cls, const char *name, A... a) {
    using Fn = R (*)(JNIEnv *, jobject, A...);
    Fn fn = (Fn)native(cls, name);
    if (!fn) return R();
    return fn(env(), self, a...);
}
}  // namespace fakejvm
