// SPDX-License-Identifier: CC0-1.0
// Windows build: the program. Opens the window with SDL2, starts the host (JNI_OnLoad, as when
// Android loads libcqhost.so), then does what the game's Java code does on Android: hands the game
// its activity and APK (CqActivity.onCreate), reports the window (SDLSurface) and runs the game's
// main loop on its own thread (SDLMain). The main thread turns SDL input into the game's Java input
// calls: keys as Android key codes, the mouse as touches, controllers through the virtual pad.
//
//   cq64.exe [path to your Costume Quest APK]     (default: CostumeQuest.apk next to cq64.exe)
#include "console_win.h"
#include "fakejvm.h"

#include "../common.h"
#include "../paths.h"

#include <windows.h>
#include <commdlg.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <GLES2/gl2.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <map>
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

// The guest runtime (libguestrt.so) is built into cq64.exe (guest_rt.S).
extern "C" const uint8_t cq_guest_rt_start[], cq_guest_rt_end[];
bool builtin_lib(const char *soname, std::vector<uint8_t> &out) {
    if (strcmp(soname, "libguestrt.so") != 0) return false;
    out.assign(cq_guest_rt_start, cq_guest_rt_end);
    return true;
}

// Screenshots of the game's own frame (F12, or a file named "screenshot" in the files directory):
// saved as screenshot.bmp next to cq64.exe.
void before_swap() {
    static int frame = 0;
    // GL errors since the last frame (each code logged a few times): what ANGLE rejects.
    static std::map<GLenum, int> errors;
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR;)
        if (errors[e]++ < 3) LOGW("GL error 0x%x before frame %d", e, frame);
    winconsole::draw(g_win_w, g_win_h);
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

// The controller support DevTools.java gives the Android build: every controller drives the
// virtual pad (the one used last), the menus switch to their controller look (highlight, button
// prompts) while one is connected, button icons follow the controller (PlayStation icons for Sony
// pads), and Escape on the keyboard works as B (back / cancel), which the menus need.
const char *const kTouchModeOverride =  // DevTools.TOUCH_MODE_OVERRIDE
    "if input and not _cqIsTouchMode then "
    "_cqIsTouchMode = input.IsTouchMode; _cqIsGamepadMode = input.IsGamepadMode; "
    "input.IsTouchMode = function() if _cqPad then return false end return _cqIsTouchMode() end; "
    "input.IsGamepadMode = function() if _cqPad then return true end return _cqIsGamepadMode() end end";

void dev_command(const char *lua) {  // run as is (\x01), not echoed in the debug console
    if (fakejvm::native(DEVTOOLS, "nativeCommand"))
        fakejvm::call_static<void>(DEVTOOLS, "nativeCommand",
                                   (jobject)fakejvm::new_string((std::string("\x01") + lua).c_str()));
}

// Buttons held by the test input ("pad a 200", see debug_input), sent with the real controller's.
std::atomic<jint> g_test_buttons{0};
std::atomic<jint> g_test_axes[6];  // held by the test input ("stick ry 1 500"): lx ly rx ry lt rt
std::atomic<bool> g_test_touched{false};  // a test "tap" happened
const Uint32 kPadTestEvent = SDL_RegisterEvents(1);

struct Pad {
    std::vector<SDL_GameController *> open;
    SDL_GameController *last = nullptr;  // the controller used last drives the pad
    jint keys = 0;                       // pad buttons held on the keyboard (Escape = B)
    int reported = -1, icons = -1;
    jint sent[7] = {-1};  // last values given to the game
    static constexpr jint kStickDeadZone = 6000, kTriggerDeadZone = 3000;  // of 32767

    bool supported() const { return fakejvm::native(DEVTOOLS, "nativePad") != nullptr; }  // optimized variant
    // Controller mode follows the input used last: on while a controller is connected, off after
    // a touch until the next controller input. Reporting it for as long as a controller was merely
    // connected left the battle HUD's ability button on Y after switching to touch.
    bool touched = false;
    void touch() {
        touched = true;
        report();
    }
    void report() {
        int any = open.empty() || touched ? 0 : 1;
        if (any != reported) {
            reported = any;
            dev_command(any ? "_cqPad = true" : "_cqPad = false");
        }
    }
    void use(SDL_GameController *gc) {
        last = gc;
        SDL_GameControllerType t = SDL_GameControllerGetType(gc);
        int set = t == SDL_CONTROLLER_TYPE_PS3 || t == SDL_CONTROLLER_TYPE_PS4 || t == SDL_CONTROLLER_TYPE_PS5 ? 1 : 0;
        if (set != icons && fakejvm::native(DEVTOOLS, "nativePadIcons")) {
            icons = set;
            fakejvm::call_static<void>(DEVTOOLS, "nativePadIcons", (jint)set);
        }
    }
    void added(int index) {
        static const bool ignore = getenv("CQ64_IGNORE_CONTROLLERS") != nullptr;  // testing as if none were connected
        if (ignore) return;
        SDL_GameController *gc = SDL_GameControllerOpen(index);
        if (!gc) return;
        LOGI("controller: %s (type %d)", SDL_GameControllerName(gc), (int)SDL_GameControllerGetType(gc));
        open.push_back(gc);
        if (!last) use(gc);
        report();
    }
    void removed(SDL_JoystickID id) {
        for (size_t i = 0; i < open.size(); i++)
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(open[i])) == id) {
                if (open[i] == last) last = nullptr;
                SDL_GameControllerClose(open[i]);
                open.erase(open.begin() + i);
                break;
            }
        if (!last && !open.empty()) use(open[0]);
        report();
        send();
    }
    void event(SDL_JoystickID id) {
        for (SDL_GameController *gc : open)
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc)) == id && gc != last) use(gc);
        send();
    }
    bool combo = false;  // LB + RB + A held: opens / closes the debug console
    void send() {
        SDL_GameController *gc = last;
        jint buttons = keys | g_test_buttons.load();
        if (gc)
            for (int b = 0; b <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; b++)
                if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) buttons |= 1 << b;
        constexpr jint kShoulders = 1 << SDL_CONTROLLER_BUTTON_LEFTSHOULDER | 1 << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
        bool held = (buttons & kShoulders) == kShoulders && (buttons & 1 << SDL_CONTROLLER_BUTTON_A);
        if (held && !combo) winconsole::toggle();
        combo = held;
        if ((buttons & kShoulders) == kShoulders) buttons &= ~(1 << SDL_CONTROLLER_BUTTON_A);  // A stays with the console
        using Fn = void (*)(JNIEnv *, jclass, jint, jint, jint, jint, jint, jint, jint);
        auto fn = (Fn)fakejvm::native(DEVTOOLS, "nativePad");
        if (!fn) return;
        auto axis = [&](SDL_GameControllerAxis a) { return gc ? (jint)SDL_GameControllerGetAxis(gc, a) : 0; };
        // Android reports a resting stick as exactly 0 (each axis has a "flat" range it filters);
        // SDL passes the jitter of a resting stick on. Any change counts as controller use for the
        // game, so without a dead zone it keeps switching the HUD back to controller icons after
        // a touch. Same for triggers resting slightly pressed.
        auto stick = [&](SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, jint &x, jint &y) {
            x = axis(ax), y = axis(ay);
            if ((long)x * x + (long)y * y < (long)kStickDeadZone * kStickDeadZone) x = y = 0;
        };
        auto trigger = [&](SDL_GameControllerAxis a) { jint v = axis(a); return v < kTriggerDeadZone ? 0 : v; };
        jint v[7] = {buttons};
        stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, v[1], v[2]);
        stick(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, v[3], v[4]);
        v[5] = trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT), v[6] = trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        for (int i = 0; i < 6; i++)
            if (jint t = g_test_axes[i].load()) v[1 + i] = t;
        if (!memcmp(v, sent, sizeof v)) return;  // nothing the game would see changed
        memcpy(sent, v, sizeof v);
        if (touched && (v[0] | v[1] | v[2] | v[3] | v[4] | v[5] | v[6])) {
            LOGI("controller input after a touch (%s): buttons %x sticks %d,%d %d,%d triggers %d %d",
                 gc ? SDL_GameControllerName(gc) : "-", v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
            touched = false;
            report();
        }
        fn(fakejvm::env(), fakejvm::find_class(DEVTOOLS), v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    }
};
}  // namespace

// FMODAudioDevice.run(): FMOD Ex on Android doesn't drive the sound card itself; a Java thread asks
// the mixer for its format (fmodGetInfo), then pulls 16-bit stereo blocks (fmodProcess) and
// writes them to an AudioTrack whose blocking write paces the loop. Here they go to an SDL audio
// queue kept about as deep as FMOD's own buffers.
void fmod_audio() {
    constexpr const char *FMOD = "org/fmod/FMODAudioDevice";
    enum { INFO_SAMPLERATE, INFO_DSPBUFFERLENGTH, INFO_DSPNUMBUFFERS, INFO_MIXERRUNNING };
    jobject device = fakejvm::new_object(FMOD);
    auto info = [&](jint what) { return fakejvm::call<jint>(device, FMOD, "fmodGetInfo", what); };
    for (;;) {
        jint rate = info(INFO_SAMPLERATE), frames = info(INFO_DSPBUFFERLENGTH), count = info(INFO_DSPNUMBUFFERS);
        if (rate <= 0 || frames <= 0 || count <= 0) {
            SDL_Delay(100);
            continue;
        }
        const u32 bytes = (u32)frames * 2 * 2;
        // In guest memory, so the game's GetDirectBufferAddress needs no copy.
        u32 gbuf = mem::halloc(bytes, 16);
        jobject buffer = fakejvm::new_direct_buffer(g2h(gbuf), bytes);
        SDL_AudioSpec want{}, have{};
        want.freq = rate;
        want.format = AUDIO_S16LSB;
        want.channels = 2;
        want.samples = (Uint16)frames;
        SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!dev) {
            LOGE("audio: %s", SDL_GetError());
            return;
        }
        LOGI("audio: FMOD mixer %d Hz, %d x %d frames", rate, count, frames);
        SDL_PauseAudioDevice(dev, 0);
        while (info(INFO_MIXERRUNNING) == 1) {
            while (SDL_GetQueuedAudioSize(dev) > bytes * (u32)count) SDL_Delay(1);
            fakejvm::call<jint>(device, FMOD, "fmodProcess", buffer);
            SDL_QueueAudio(dev, g2h(gbuf), bytes);
        }
        LOGI("audio: FMOD mixer stopped");
        SDL_CloseAudioDevice(dev);
        mem::hfree(gbuf);
    }
}

// Test input without a keyboard or mouse: lines in files/input such as "key 66" (an Android key
// code, pressed and released), "tap 0.5 0.4" (a touch at that fraction of the window) or
// "wait 500" (milliseconds) or "pad a 200" (a virtual controller button held that long: a b x y
// back start ls rs lb rb up down left right; "pad lb+rb+a" holds several), "stick ry 1 500" (a stick
// axis lx ly rx ry held at -1..1 that long; y down is positive; lt rt for the triggers, 0..1;
// "lt+rt" holds several). "sdlkey Return" (an SDL key name: ` F1 Tab Up ...)
// and "type <text>" go through the window's own event queue, as a real keyboard would (the
// debug console's path).
void debug_input() {
    const std::string path = win::files_dir() + "/input";
    for (;;) {
        SDL_Delay(200);
        FILE *f = fopen(path.c_str(), "r");
        if (!f) continue;
        std::vector<std::string> lines;
        char line[256];
        while (fgets(line, sizeof line, f)) lines.push_back(line);
        fclose(f);
        unlink(path.c_str());
        for (const std::string &l : lines) {
            LOGI("test input: %.*s", (int)l.find_last_not_of("\r\n") + 1, l.c_str());
            int k, ms;
            float x, y;
            if (sscanf(l.c_str(), "key %d", &k) == 1) {
                fakejvm::call_static<void>(SDLA, "onNativeKeyDown", (jint)k);
                SDL_Delay(120);
                fakejvm::call_static<void>(SDLA, "onNativeKeyUp", (jint)k);
            } else if (sscanf(l.c_str(), "tap %f %f", &x, &y) == 2) {
                SDL_Event e{};
                e.type = kPadTestEvent;
                g_test_touched = true;
                SDL_PushEvent(&e);
                fakejvm::call_static<void>(SDLA, "onNativeTouch", (jint)0, (jint)0, (jint)0, (jfloat)x, (jfloat)y, 1.0f);
                SDL_Delay(120);
                fakejvm::call_static<void>(SDLA, "onNativeTouch", (jint)0, (jint)0, (jint)1, (jfloat)x, (jfloat)y, 1.0f);
            } else if (char name[16]; sscanf(l.c_str(), "pad %15s %d", name, &ms) >= 1) {
                static const char *const names[] = {"a", "b", "x", "y", "back", "guide", "start", "ls", "rs",
                                                    "lb", "rb", "up", "down", "left", "right"};
                int mask = 0;  // "lb+rb+a": several at once
                for (char *t = strtok(name, "+"); t; t = strtok(nullptr, "+"))
                    for (int i = 0; i < 15; i++)
                        if (!strcmp(t, names[i])) mask |= 1 << i;
                if (!mask) continue;
                if (sscanf(l.c_str(), "pad %*s %d", &ms) != 1) ms = 120;
                SDL_Event e{};
                e.type = kPadTestEvent;
                g_test_buttons |= mask;
                SDL_PushEvent(&e);
                SDL_Delay(ms);
                g_test_buttons &= ~mask;
                SDL_PushEvent(&e);
            } else if (char ax[16]; sscanf(l.c_str(), "stick %15s %f %d", ax, &x, &ms) >= 2) {
                static const char *const axes[] = {"lx", "ly", "rx", "ry", "lt", "rt"};
                int mask = 0;  // "lt+rt": several at once
                for (char *t = strtok(ax, "+"); t; t = strtok(nullptr, "+"))
                    for (int i = 0; i < 6; i++)
                        if (!strcmp(t, axes[i])) mask |= 1 << i;
                if (!mask) continue;
                if (sscanf(l.c_str(), "stick %*s %*f %d", &ms) != 1) ms = 300;
                SDL_Event e{};
                e.type = kPadTestEvent;
                for (int i = 0; i < 6; i++)
                    if (mask & 1 << i) g_test_axes[i] = (jint)(std::clamp(x, -1.0f, 1.0f) * 32767);
                SDL_PushEvent(&e);
                SDL_Delay(ms);
                for (int i = 0; i < 6; i++)
                    if (mask & 1 << i) g_test_axes[i] = 0;
                SDL_PushEvent(&e);
            } else if (char key[32]; sscanf(l.c_str(), "sdlkey %31s", key) == 1) {
                SDL_Event e{};
                e.type = SDL_KEYDOWN;
                e.key.state = SDL_PRESSED;
                e.key.keysym.sym = SDL_GetKeyFromName(key);
                if (e.key.keysym.sym == SDLK_UNKNOWN) continue;
                SDL_PushEvent(&e);
                SDL_Delay(60);
                e.type = SDL_KEYUP;
                e.key.state = SDL_RELEASED;
                SDL_PushEvent(&e);
            } else if (l.rfind("type ", 0) == 0) {
                std::string t = l.substr(5, l.find_last_not_of("\r\n") + 1 - 5);
                for (size_t i = 0; i < t.size(); i += sizeof(SDL_TextInputEvent::text) - 1) {
                    SDL_Event e{};
                    e.type = SDL_TEXTINPUT;
                    std::string part = t.substr(i, sizeof(e.text.text) - 1);
                    memcpy(e.text.text, part.c_str(), part.size() + 1);
                    SDL_PushEvent(&e);
                }
            } else if (sscanf(l.c_str(), "wait %d", &ms) == 1) {
                SDL_Delay(ms);
            }
            SDL_Delay(300);
        }
    }
}

std::string pick_apk() {
    char buf[MAX_PATH] = "";
    OPENFILENAMEA o{};
    o.lStructSize = sizeof o;
    o.lpstrFilter = "Costume Quest for Android (*.apk)\0*.apk\0All files\0*.*\0";
    o.lpstrFile = buf;
    o.nMaxFile = sizeof buf;
    o.lpstrTitle = "Select your Costume Quest APK";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&o) ? buf : "";
}

std::string read_line(const std::string &path) {
    std::string r;
    if (FILE *f = fopen(path.c_str(), "r")) {
        char buf[MAX_PATH * 2] = "";
        if (fgets(buf, sizeof buf, f)) r = buf;
        fclose(f);
    }
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
    return r;
}

bool exists(const std::string &path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The user's APK: dropped on cq64.exe (or passed), else the one used last time, else
// CostumeQuest.apk next to cq64.exe, else chosen in a file dialog. Remembered in cq64-data/apk.txt.
// Everything the game needs is read from it: its ARM libraries and its data.
std::string choose_apk(int argc, char **argv, const std::string &dir) {
    const std::string saved = dir + "/cq64-data/apk.txt";
    std::string apk = argc > 1 ? argv[1] : read_line(saved);
    if (!apk.empty() && !exists(apk)) apk.clear();
    if (apk.empty() && exists(dir + "/CostumeQuest.apk")) apk = dir + "/CostumeQuest.apk";
    for (;;) {
        if (apk.empty()) apk = pick_apk();
        if (apk.empty()) return "";  // cancelled
        for (char &c : apk)
            if (c == '\\') c = '/';
        if (fakejvm::open_apk(apk) && fakejvm::apk_has("lib/armeabi-v7a/libCq.so")) break;
        std::string msg = apk + "\n\nThis isn't Costume Quest for Android (it has no lib/armeabi-v7a/libCq.so). "
                                "Choose the game's APK.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "CQ64", msg.c_str(), nullptr);
        apk.clear();
    }
    CreateDirectoryA((dir + "/cq64-data").c_str(), nullptr);
    if (FILE *f = fopen(saved.c_str(), "w")) {
        fprintf(f, "%s\n", apk.c_str());
        fclose(f);
    }
    return apk;
}

int main(int argc, char **argv) {
    _set_fmode(_O_BINARY);  // the game's files are binary; never translate line endings
    const std::string dir = exe_dir();
    win::g_files = dir + "/cq64-data/files";
    cq_log_open((dir + "/cq64.log").c_str());
    win::g_apk = choose_apk(argc, argv, dir);
    if (win::g_apk.empty()) return 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        LOGE("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    int w = 1280, h = 720;
    SDL_Window *window = SDL_CreateWindow("Costume Quest Mobile", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w,
                                          h, SDL_WINDOW_SHOWN);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info)) {
        LOGE("window: %s", SDL_GetError());
        return 1;
    }
    // The program's icon (resource 1, if built with one) for the title bar and taskbar.
    for (int big : {1, 0}) {
        int px = GetSystemMetrics(big ? SM_CXICON : SM_CXSMICON);
        if (HANDLE ico = LoadImageA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(1), IMAGE_ICON, px, px, 0))
            SendMessageA(info.info.win.window, WM_SETICON, big ? ICON_BIG : ICON_SMALL, (LPARAM)ico);
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

    std::thread(fmod_audio).detach();
    std::thread(debug_input).detach();

    Pad pad;
    dev_command(kTouchModeOverride);  // queued until the game's Lua console exists
    pad.report();
    bool mouse_down = false;
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        if (winconsole::handle_event(e)) continue;  // the debug console (` or F1) has the keyboard
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
            if (e.key.keysym.sym == SDLK_ESCAPE && pad.supported()) {  // Escape = B: back / cancel
                pad.keys = e.type == SDL_KEYDOWN ? pad.keys | (1 << SDL_CONTROLLER_BUTTON_B)
                                                 : pad.keys & ~(1 << SDL_CONTROLLER_BUTTON_B);
                pad.send();
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
            if (action == 0) pad.touch();
            mouse_down = action != 1;
            int x = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
            int y = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            call_static<void>(SDLA, "onNativeTouch", (jint)0, (jint)0, (jint)action, (jfloat)x / w, (jfloat)y / h,
                              (jfloat)1.0f);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            pad.added(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            pad.removed(e.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            pad.event(e.cbutton.which);
            break;
        case SDL_CONTROLLERAXISMOTION:
            pad.event(e.caxis.which);
            break;
        default:
            if (e.type == kPadTestEvent) {
                if (g_test_touched.exchange(false)) pad.touch();
                pad.send();
            }
            break;
        }
    }
}
