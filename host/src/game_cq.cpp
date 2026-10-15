// SPDX-License-Identifier: CC0-1.0
// Costume Quest (com.doublefine.cq): SDL-based Double Fine engine in libCq.so.
//
// Built in two variants (CMake CQ_VARIANT): "optimized" adds controller support (gamepad.cpp),
// patches to the data pack (datapatch.cpp) and debug hooks driven from adb; "original"
// (CQ_ORIGINAL) only does what running the 32-bit game on a 64-bit-only phone requires, so it
// plays exactly like the 32-bit APK.
#include "datapatch.h"
#include "devtools.h"
#include "game.h"
#include "gamepad.h"
#include "simrate.h"
#include "loader.h"
#include "natives.h"
#include "paths.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

namespace game {
const char *const kPackage = "com.doublefine.cq";
const std::vector<GuestLib> kLibs = {
    {"libguest_rt.so", "libguestrt.so"},
    {"libguest_fmodex.so", "libfmodex.so"},
    {"libguest_fmodevent.so", "libfmodevent.so"},
    {"libguest_Cq.so", "libCq.so"},
};

void setup() {
    // The game keeps config and saves under /sdcard/dfp, which modern Android denies to apps
    // without storage permission; redirect the old shared-storage roots into private storage.
    const std::string storage = files_dir() + "/sdcard";
    host::mkdirs(storage + "/dfp/cq-droid/_write");
    for (const char *root : {"/sdcard/", "/mnt/sdcard/", "/storage/emulated/0/"})
        paths::add_mapping(root, storage + "/");

    // The game reads its 1 GB data pack (assets/ant.mp3) out of its own APK. A slim APK without it
    // is used when the phone lacks room for the full one; the pack then lives in a zip streamed
    // into private storage (see install_split.sh), and reads of the APK are redirected there.
    const std::string data_zip = files_dir() + "/cqdata.zip";
    if (access(data_zip.c_str(), R_OK) == 0) {
        std::string apk = host::own_apk();
        if (!apk.empty()) paths::add_mapping(apk, data_zip);
        else LOGW("data zip present but own APK path not found");
    }
#ifndef CQ_ORIGINAL
    datapatch::init(access(data_zip.c_str(), R_OK) == 0 ? data_zip : host::own_apk());
#endif
}

#ifdef CQ_ORIGINAL
void after_load(Module *) {}
#else
void after_load(Module *main) {
    gamepad::install(main);
    simrate::install(main);  // before the game initializes
}
#endif

void register_natives(JNIEnv *env) {
    devtools::register_natives(env);  // no-op without the DevTools class (original variant)
#ifndef CQ_ORIGINAL
    gamepad::register_natives(env);
#endif
}

// Debug aids:
//   echo 'cmd' | adb shell run-as com.doublefine.cq sh -c 'cat > files/console'   runs console commands
//   echo '1 150' | ... sh -c 'cat > files/pad'   presses virtual pad buttons in turn: a mask or a
//                                                 button name (a b x y back guide start ls rs lb rb
//                                                 up down left right), then milliseconds
//   ... sh -c 'echo 1 > files/padauto'   plays battle prompts with synthetic pad input (0: off);
//                                        'echo 1 5 > ...' also makes every prompt BattlePrompt 5 (-1: off)
#ifdef CQ_ORIGINAL
void poll_debug() {}
#else
void poll_debug() {
    const std::string autof = files_dir() + "/padauto";
    if (FILE *f = fopen(autof.c_str(), "r")) {
        int on = 0;
        if (fscanf(f, "%d", &on) == 1) gamepad::set_auto(on != 0), LOGI("gamepad: auto prompts %s", on ? "on" : "off");
        int forced;
        if (fscanf(f, "%d", &forced) == 1) gamepad::force_prompt(forced), LOGI("gamepad: forced prompt %d", forced);
        fclose(f);
        unlink(autof.c_str());
    }
    const std::string pad = files_dir() + "/pad", console = files_dir() + "/console";
    if (FILE *f = fopen(pad.c_str(), "r")) {
        static const char *const names[] = {"a", "b", "x", "y", "back", "guide", "start", "ls",
                                            "rs", "lb", "rb", "up", "down", "left", "right"};
        char what[16];
        int ms;
        while (fscanf(f, "%15s %d", what, &ms) == 2) {
            char *end;
            unsigned mask = strtoul(what, &end, 0);
            if (*end) {
                mask = 0;
                for (int i = 0; i < 15; i++)
                    if (!strcasecmp(what, names[i])) mask = 1u << i;
            }
            if (mask) gamepad::debug_press(mask, ms);
            usleep(300 * 1000);
        }
        fclose(f);
        unlink(pad.c_str());
    }
    if (FILE *f = fopen(console.c_str(), "r")) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            std::string cmd(line);
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
            devtools::submit(cmd);
        }
        fclose(f);
        unlink(console.c_str());
    }
}
#endif
}  // namespace game

constexpr const char *SDL = "org/libsdl/app/SDLActivity";
constexpr const char *CQ = "com/doublefine/cq/CqActivity";
NativeDef g_natives[] = {
    N(0, SDL, "nativeFlipBuffers", "()V", void),
    N(1, SDL, "nativeInit", "()V", void),
    N(2, SDL, "nativeLowMemory", "()V", void),
    N(3, SDL, "nativePause", "()V", void),
    N(4, SDL, "nativePause2", "()V", void),
    N(5, SDL, "nativeQuit", "()V", void),
    N(6, SDL, "nativeResume", "()V", void),
    N(7, SDL, "nativeResume2", "()V", void),
    N(8, SDL, "nativeSetMaxScreenResolution", "(II)V", void, jint, jint),
    N(9, SDL, "nativeUpdateScaledWindowBufferSize", "(IILjava/lang/Object;)V", void, jint, jint, O),
    N(10, SDL, "onNativeAccel", "(FFF)V", void, jfloat, jfloat, jfloat),
    N(11, SDL, "onNativeKeyDown", "(I)V", void, jint),
    N(12, SDL, "onNativeKeyUp", "(I)V", void, jint),
    N(13, SDL, "onNativeKeyboardFocusLost", "()V", void),
    N(14, SDL, "onNativeResize", "(III)V", void, jint, jint, jint),
    N(15, SDL, "onNativeSurfaceChanged", "()V", void),
    N(16, SDL, "onNativeSurfaceDestroyed", "()V", void),
    N(17, SDL, "onNativeTouch", "(IIIFFF)V", void, jint, jint, jint, jfloat, jfloat, jfloat),
    N(18, "org/libsdl/app/SDLInputConnection", "nativeCommitText", "(Ljava/lang/String;I)V", void, O, jint),
    N(19, "org/libsdl/app/SDLInputConnection", "nativeSetComposingText", "(Ljava/lang/String;I)V", void, O, jint),
    N(20, "org/fmod/FMODAudioDevice", "fmodGetInfo", "(I)I", jint, jint),
    N(21, "org/fmod/FMODAudioDevice", "fmodProcess", "(Ljava/nio/ByteBuffer;)I", jint, O),
    N(22, CQ, "dfNativeInit",
      "(Landroid/app/Activity;Landroid/content/res/AssetManager;Ljava/lang/String;Ljava/lang/String;)V", void, O, O,
      O, O),
    N(23, CQ, "dfNativeSetLanguage", "(Ljava/lang/String;)V", void, O),
    N(24, CQ, "dfNativeShutdown", "()V", void),
    N(25, CQ, "dfTrialPurchaseComplete", "(Z)V", void, jboolean),
    N(26, CQ, "setDeviceInfo",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V",
      void, O, O, O, O, O, O, jboolean),
};
const size_t g_native_count = sizeof g_natives / sizeof g_natives[0];
