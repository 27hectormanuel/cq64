// SPDX-License-Identifier: CC0-1.0
#pragma once
// Per-game profile. The host core (loader, CPU, thunks, JNI bridge) is game-independent; one
// game_<name>.cpp, chosen at build time (CMake GAME=...), supplies what differs between games.
#include "common.h"

#include <jni.h>
#include <string>
#include <vector>

struct Module;

namespace game {
struct GuestLib {
    const char *file;    // shipped next to libcqhost, e.g. "libguest_Cq.so"
    const char *soname;  // name the other libraries import it by, e.g. "libCq.so"
};

// Android package name, e.g. "com.doublefine.cq".
extern const char *const kPackage;
// Guest libraries in load order (the guest runtime comes first). The last one is the game.
extern const std::vector<GuestLib> kLibs;

// Private files directory, /data/data/<package>/files.
std::string files_dir();
// A debug switch, read once when it is first used: the environment variable CQ64_<NAME>
// (convenient on Windows) or else the file files/<name> (on Android:
// adb shell run-as <package> sh -c 'echo VALUE > files/<name>'). Returns its value with trailing
// whitespace removed, "1" if it is set but empty, or "" if it is not set.
std::string debug_setting(const char *name);

// Before the guest libraries are loaded: path mappings and similar setup.
void setup();
// After the libraries are loaded and initialized, before the game's JNI_OnLoad.
void after_load(Module *main);
// After the game's native methods are registered: extra Java-facing natives.
void register_natives(JNIEnv *env);
// Called about once a second on a background thread, for adb debug triggers.
void poll_debug();
}  // namespace game

// Core helpers available to the profiles (host_main.cpp).
namespace host {
// The installed APK path (…/<package>-…/base.apk), as mapped into this process.
std::string own_apk();
// mkdir -p.
void mkdirs(const std::string &path);
}  // namespace host
