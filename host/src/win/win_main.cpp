// SPDX-License-Identifier: CC0-1.0
// Windows build: the program. Opens the window with SDL2, starts the host (JNI_OnLoad, as when
// Android loads libcqhost.so), then does what the game's Java code does on Android: hands the game
// its activity and APK (CqActivity.onCreate), reports the window (SDLSurface) and runs the game's
// main loop on its own thread (SDLMain). The main thread turns SDL input into the game's Java input
// calls: keys as Android key codes, the mouse as touches, controllers through the virtual pad.
//
//   cq64.exe [path to your Costume Quest APK]     (default: CostumeQuest.apk next to cq64.exe)
#include "fakejvm.h"

#include "../common.h"
#include "../paths.h"

#include <windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <GLES2/gl2.h>
#include <fcntl.h>

#include <atomic>
#include <cstdlib>
#include <vector>
#include <string>
#include <thread>

extern "C" void cq_log_open(const char *path);
extern "C" jint JNI_OnLoad(JavaVM *vm, void *);

namespace win {
namespace {
std::string g_files, g_apk;
}
std::string files_dir() { return g_files; }
std::string apk_path() { return g_apk; }
const char *const kGuestApk = "/data/app/com.doublefine.cq-1/base.apk";

namespace {
std::atomic<bool> g_shot_requested{false};
std::atomic<int> g_win_w{1280}, g_win_h{720};
}

// Screenshots of the game's own frame (F12, or a file named "screenshot" in the files directory):
// saved as screenshot.bmp next to cq64.exe.
void before_swap() {
    static int frame = 0;
    if (++frame % 30 == 0 && access((g_files + "/screenshot").c_str(), F_OK) == 0) {
        unlink((g_files + "/screenshot").c_str());
        g_shot_requested = true;
    }
    if (!g_shot_requested.exchange(false)) return;
    const int w = g_win_w, h = g_win_h;
    std::vector<uint8_t> px((size_t)w * h * 4), flipped(px.size());
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++) memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(flipped.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    std::string path = g_files.substr(0, g_files.rfind("/cq64-data")) + "/screenshot.bmp";
    if (s && SDL_SaveBMP(s, path.c_str()) == 0) LOGI("screenshot saved: %s", path.c_str());
    SDL_FreeSurface(s);
}
}  // namespace win

namespace {
constexpr const char *SDLA = "org/libsdl/app/SDLActivity";
constexpr const char *CQ = "com/doublefine/cq/CqActivity";
constexpr const char *DEVTOOLS = "com/doublefine/cq/DevTools";
constexpr jint kFormatRGBA8888 = 0x16462004;  // SDL_PIXELFORMAT_RGBA8888, as SDLSurface reports it

std::string exe_dir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, sizeof buf);
    std::string p(buf);
    for (char &c : p)
        if (c == '\\') c = '/';
    return p.substr(0, p.rfind('/'));
}

// SDL keys -> Android key codes (what SDLActivity.onNativeKeyDown passes on).
int android_key(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 29 + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return 7 + (k - SDLK_0);
    switch (k) {
    case SDLK_UP: return 19;
    case SDLK_DOWN: return 20;
    case SDLK_LEFT: return 21;
    case SDLK_RIGHT: return 22;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return 66;
    case SDLK_ESCAPE: return 4;  // BACK
    case SDLK_SPACE: return 62;
    case SDLK_TAB: return 61;
    case SDLK_BACKSPACE: return 67;
    case SDLK_LSHIFT: return 59;
    case SDLK_RSHIFT: return 60;
    case SDLK_COMMA: return 55;
    case SDLK_PERIOD: return 56;
    case SDLK_MINUS: return 69;
    case SDLK_EQUALS: return 70;
    case SDLK_SLASH: return 76;
    default: return 0;
    }
}

struct Pad {
    SDL_GameController *gc = nullptr;
    void send() {
        using Fn = void (*)(JNIEnv *, jclass, jint, jint, jint, jint, jint, jint, jint);
        auto fn = (Fn)fakejvm::native(DEVTOOLS, "nativePad");
        if (!fn) return;  // the original variant has no controller support
        jint buttons = 0;
        if (gc)
            for (int b = 0; b <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; b++)
                if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) buttons |= 1 << b;
        auto axis = [&](SDL_GameControllerAxis a) { return gc ? (jint)SDL_GameControllerGetAxis(gc, a) : 0; };
        fn(fakejvm::env(), fakejvm::find_class(DEVTOOLS), buttons, axis(SDL_CONTROLLER_AXIS_LEFTX),
           axis(SDL_CONTROLLER_AXIS_LEFTY), axis(SDL_CONTROLLER_AXIS_RIGHTX), axis(SDL_CONTROLLER_AXIS_RIGHTY),
           axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT), axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    }
};
}  // namespace

int main(int argc, char **argv) {
    _set_fmode(_O_BINARY);  // the game's files are binary; never translate line endings
    const std::string dir = exe_dir();
    win::g_apk = argc > 1 ? argv[1] : dir + "/CostumeQuest.apk";
    for (char &c : win::g_apk)
        if (c == '\\') c = '/';
    win::g_files = dir + "/cq64-data/files";
    cq_log_open((dir + "/cq64.log").c_str());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        LOGE("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    if (GetFileAttributesA(win::g_apk.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::string msg = "Costume Quest's APK was not found:\n" + win::g_apk +
                          "\n\nPut your CostumeQuest.apk next to cq64.exe, or pass its path.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CQ64", msg.c_str(), nullptr);
        return 1;
    }
    int w = 1280, h = 720;
    SDL_Window *window = SDL_CreateWindow("Costume Quest", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                          SDL_WINDOW_SHOWN);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info)) {
        LOGE("window: %s", SDL_GetError());
        return 1;
    }
    LOGI("cq64 for Windows: APK %s, files %s", win::g_apk.c_str(), win::g_files.c_str());

    paths::add_mapping(win::kGuestApk, win::g_apk);
    fakejvm::init(info.info.win.window, w, h);
    JNI_OnLoad(fakejvm::vm(), nullptr);  // boots the emulator, loads the game, registers natives

    using fakejvm::call_static;
    // CqActivity.onCreate
    call_static<void>(CQ, "dfNativeInit", fakejvm::activity(), fakejvm::asset_manager(), (jobject) nullptr,
                      (jobject)fakejvm::new_string(win::kGuestApk));
    call_static<void>(SDLA, "nativeSetMaxScreenResolution", (jint)w, (jint)h);
    call_static<void>(CQ, "setDeviceInfo", (jobject)fakejvm::new_string("PC"), (jobject)fakejvm::new_string("windows"),
                      (jobject)fakejvm::new_string("cq64"), (jobject)fakejvm::new_string("Windows PC"),
                      (jobject)fakejvm::new_string("cq64"), (jobject)fakejvm::new_string("10"), (jboolean)JNI_TRUE);
    // SDLSurface.surfaceCreated / surfaceChanged
    call_static<void>(SDLA, "nativeUpdateScaledWindowBufferSize", (jint)w, (jint)h, fakejvm::surface());
    call_static<void>(SDLA, "onNativeResize", (jint)w, (jint)h, kFormatRGBA8888);
    call_static<void>(SDLA, "onNativeSurfaceChanged");
    // SDLMain: the game's main loop
    std::thread([] {
        call_static<void>(SDLA, "nativeInit");
        LOGI("the game's main loop returned");
        SDL_Event e{};
        e.type = SDL_QUIT;
        SDL_PushEvent(&e);
    }).detach();

    Pad pad;
    bool mouse_down = false;
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        switch (e.type) {
        case SDL_QUIT:
            LOGI("quitting");
            call_static<void>(SDLA, "nativeQuit");
            _exit(0);
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (e.key.repeat) break;
            if (e.key.keysym.sym == SDLK_F12) {
                if (e.type == SDL_KEYDOWN) win::g_shot_requested = true;
                break;
            }
            if (int k = android_key(e.key.keysym.sym))
                call_static<void>(SDLA, e.type == SDL_KEYDOWN ? "onNativeKeyDown" : "onNativeKeyUp", (jint)k);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION: {
            if (e.type == SDL_MOUSEMOTION && !mouse_down) break;
            if (e.type != SDL_MOUSEMOTION && e.button.button != SDL_BUTTON_LEFT) break;
            int action = e.type == SDL_MOUSEBUTTONDOWN ? 0 : e.type == SDL_MOUSEBUTTONUP ? 1 : 2;
            mouse_down = action != 1;
            int x = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
            int y = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            call_static<void>(SDLA, "onNativeTouch", (jint)0, (jint)0, (jint)action, (jfloat)x / w, (jfloat)y / h,
                              (jfloat)1.0f);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            if (!pad.gc) {
                pad.gc = SDL_GameControllerOpen(e.cdevice.which);
                LOGI("controller: %s", pad.gc ? SDL_GameControllerName(pad.gc) : SDL_GetError());
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (pad.gc && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad.gc))) {
                SDL_GameControllerClose(pad.gc);
                pad.gc = nullptr;
                pad.send();
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION:
            pad.send();
            break;
        default: break;
        }
    }
}
