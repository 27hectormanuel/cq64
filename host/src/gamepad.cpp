// SPDX-License-Identifier: CC0-1.0
// Gamepad support.
//
// The engine has full SDL game controller support (SDLGamepad, bound by input.lua's
// "SDL_Gamepad" layout), but the Android SDL inside libCq only reports one joystick, the
// accelerometer (3 axes, no buttons), and nothing maps it as a game controller. So:
//   - SDL_SYS_JoystickOpen / SDL_SYS_JoystickUpdate (Thumb code in libCq, called directly, so
//     not overridable through imports) are patched to jump to host thunks that present a pad
//     with 6 axes and 15 buttons and feed it the state DevTools reports from real controllers.
//   - SDL_GAMECONTROLLERCONFIG maps that joystick as a game controller. Its GUID is the first
//     16 bytes of the backend's joystick name, "Android accelerometer". SDL_IsGameController is
//     patched too: at the engine's startup check the original still said no (it says yes later).
// PhysicalInputManager only enumerates joysticks at startup, so the pad always exists and
// reads neutral while no controller is connected.
//
// Battle prompts are touch-only on Android (tap the coloured icon, swipe, rub). The PC version
// (Cq.exe) judges the same prompts with controller buttons in Battle_HandleInput; that logic is
// ported here: Battle_HandleInput is hooked, and while a prompt is up, presses of A/B/X/Y (the
// console colours green/red/blue/yellow) and stick / d-pad flicks are judged the PC way. Touch
// still goes through the original code, so touch play is unchanged.
//   0-3   press the prompted button (any other face button fails)
//   4-7   swipe left/right/up/down: flick the stick or press the d-pad
//   8-11  mash the prompted button to fill the meter
//   12-15 press the prompted button while the meter is in its target zone
//   16-19 flick in the prompted direction while the meter is in its target zone
//   20    rub: wiggle the left stick to fill the meter
// The battle HUD picks touch or console button icons by _global.ShowPCButtons, which Android's
// HUD script sets to true once and the engine never pushes; Battle_PushFlashVars is hooked to
// push it from the engine's input mode (gamepad after controller input, touch after a touch),
// as the PC version does.
#include "gamepad.h"

#include "abi.h"
#include "datapatch.h"
#include "loader.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <strings.h>
#include <string>
#include <unordered_map>
#include <unistd.h>

namespace gamepad {
namespace {
constexpr int NAXES = 6;     // leftx, lefty, rightx, righty, lefttrigger, righttrigger
constexpr int NBUTTONS = 15;  // a b x y back guide start ls rs lb rb dpup dpdown dpleft dpright

// Button bit i of g_buttons is joystick button i, matching the mapping below.
constexpr const char *MAPPING =
    "416e64726f696420616363656c65726f,SDL_Gamepad,"
    "a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,"
    "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";

std::atomic<u32> g_buttons{0};
std::atomic<u32> g_debug_buttons{0};  // held by debug_press()
std::atomic<s32> g_axes[NAXES];
u32 g_private_axis, g_private_button;  // SDL_PrivateJoystickAxis / Button
// Battle globals in libCq (guest addresses).
struct {
    u32 state, input_state, minigame, tuning, timer, duration, progress, success, failed, bar, bar_adjusted;
    u32 is_gamepad;  // VirtualGamepadManager::m_bIsGamepad
} B;
u32 g_handle_input_orig, g_push_vars_orig;  // trampolines to the hooked originals
u32 g_set_bool;                             // FlashValueSnapshot::SetGlobalVariable(Name const&, bool)
u32 g_name_show_pc;                         // guest Name holding "ShowPCButtons"
u32 g_name_minigame_visible;                // guest Name holding "MinigameVisible"
u32 g_name_last_costume;                    // guest Name holding "_lastCurrentCostume"
std::atomic<u32> g_pressed{0};              // buttons pressed since the game thread last looked
u32 g_bind_texture_orig;
u32 g_create_image_orig;  // FlashImageCreator::CreateImage
u32 g_texture_lock;       // RsRef<Texture>::Lock() const
std::atomic<bool> g_icons_changed{false};  // icon set changed; switch_icons() on the game thread
// State last reported to SDL (game thread only). SDL_PrivateJoystickButton posts an event even
// when nothing changed, which floods SDL's event queue, so only changes are reported.
u32 g_sent_buttons;
s32 g_sent_axes[NAXES];

// SDL_bool SDL_IsGameController(int device_index): the virtual pad is the only joystick.
s32 t_is_gc(s32 index) { return index == 0; }

// int SDL_SYS_JoystickOpen(SDL_Joystick *joystick, int device_index)
s32 t_open(u32 joy, s32 index) {
    if (index != 0) return -1;
    wr<s32>(joy + 0x08, NAXES);
    wr<s32>(joy + 0x10, 0);  // hats
    wr<s32>(joy + 0x18, 0);  // balls
    wr<s32>(joy + 0x20, NBUTTONS);
    g_sent_buttons = 0;  // SDL zeroes the new joystick's state
    memset(g_sent_axes, 0, sizeof g_sent_axes);
    LOGI("gamepad: virtual pad opened");
    return 0;
}

// void SDL_SYS_JoystickUpdate(SDL_Joystick *joystick)
void switch_icons();

void t_update(u32 joy) {
    if (g_icons_changed.exchange(false)) switch_icons();
    for (int i = 0; i < NAXES; i++) {
        s32 v = g_axes[i].load(std::memory_order_relaxed);
        if (v == g_sent_axes[i]) continue;
        g_sent_axes[i] = v;
        cpu::call32(g_private_axis, {joy, (u32)i, (u32)v});
    }
    u32 buttons = g_buttons.load(std::memory_order_relaxed) | g_debug_buttons.load(std::memory_order_relaxed);
    for (u32 changed = buttons ^ g_sent_buttons; changed; changed &= changed - 1) {
        int i = __builtin_ctz(changed);
        cpu::call32(g_private_button, {joy, (u32)i, (buttons >> i) & 1});
    }
    g_sent_buttons = buttons;
}

// Replaces the Thumb function at `fn` with "ldr.w pc, [pc, #0]; .word thunk".
void redirect(Module *cq, const char *name, u32 thunk) {
    u32 fn = loader::lookup_in(cq, name);
    if (!fn) fatal("gamepad: %s not found", name);
    u32 at = fn & ~1u;
    if (!(fn & 1) || (at & 3)) fatal("gamepad: unexpected entry for %s: 0x%x", name, fn);
    wr<u16>(at, 0xF8DF);
    wr<u16>(at + 2, 0xF000);
    wr<u32>(at + 4, thunk);  // ARM thunk: bit 0 clear switches out of Thumb
}

// Replaces the ARM function at `fn` with "ldr pc, [pc, #-4]; .word thunk" and returns a
// trampoline running its first two (position-independent) instructions, then the rest.
u32 hook_arm(Module *cq, const char *name, u32 thunk) {
    u32 fn = loader::lookup_in(cq, name);
    if (!fn || (fn & 3)) fatal("gamepad: unexpected entry for %s: 0x%x", name, fn);
    u32 tramp = mem::halloc(16, 4);
    wr<u32>(tramp, rd<u32>(fn));
    wr<u32>(tramp + 4, rd<u32>(fn + 4));
    wr<u32>(tramp + 8, 0xE51FF004);  // ldr pc, [pc, #-4]
    wr<u32>(tramp + 12, fn + 8);
    wr<u32>(fn, 0xE51FF004);
    wr<u32>(fn + 4, thunk);
    return tramp;
}

// Game-thread state of the prompt handler.
u32 g_prev_dirs;           // stick/d-pad directions held last frame
float g_rub_x, g_rub_y;    // stick position last frame, for rubbing
bool g_rub_valid;

constexpr u32 BTN_DPUP = 11, BTN_DPDOWN = 12, BTN_DPLEFT = 13, BTN_DPRIGHT = 14;
enum { DIR_LEFT = 1, DIR_RIGHT = 2, DIR_UP = 4, DIR_DOWN = 8 };

std::atomic<bool> g_debug_stick{false};  // debug: left stick from g_debug_lx/ly
std::atomic<s32> g_debug_lx{0}, g_debug_ly{0};
std::atomic<bool> g_auto{false};          // debug: play battle prompts automatically
std::atomic<int> g_force_prompt{-1};      // debug: BattlePrompt to use for every prompt (-1: off)
u32 g_prepare_orig;                       // Battle_PrepareMiniGame trampoline

float axis(int i) {
    if (i < 2 && g_debug_stick.load(std::memory_order_relaxed))
        return (i ? g_debug_ly : g_debug_lx).load(std::memory_order_relaxed) / 32767.0f;
    return g_axes[i].load(std::memory_order_relaxed) / 32767.0f;
}

// Directions held now: the left stick past 0.6 (dominant axis), or the d-pad.
u32 held_dirs(u32 buttons, u32 prev) {
    u32 d = 0;
    float x = axis(0), y = axis(1);  // SDL: y grows downwards
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    float on = (prev & 15) ? 0.4f : 0.6f;  // hysteresis
    if (ax > on || ay > on) d |= ax > ay ? (x < 0 ? DIR_LEFT : DIR_RIGHT) : (y < 0 ? DIR_UP : DIR_DOWN);
    if (buttons & (1u << BTN_DPLEFT)) d |= DIR_LEFT;
    if (buttons & (1u << BTN_DPRIGHT)) d |= DIR_RIGHT;
    if (buttons & (1u << BTN_DPUP)) d |= DIR_UP;
    if (buttons & (1u << BTN_DPDOWN)) d |= DIR_DOWN;
    return d;
}

// The meter prompts' target zone, as in both versions: 0x45..0x52 on the progress bar.
void judge_timing() {
    if (rd<s32>(B.bar) < 0x45 || rd<s32>(B.bar_adjusted) > 0x52) {
        wr<u8>(B.failed, 1);
    } else {
        if (rd<s32>(B.bar) > 0x51) wr<s32>(B.bar, 0x52);
        wr<u8>(B.success, 1);
    }
}

void add_progress(float amount) {
    float p = rd<float>(B.progress) + amount;
    if (p >= 1.0f) {
        p = 1.0f;
        wr<u8>(B.success, 1);
    }
    wr<float>(B.progress, p);
}

// Controller input for an active prompt (g_BattleInputState 6), ported from Cq.exe.
void pad_prompt(u32 pressed, u32 new_dirs) {
    if (rd<float>(B.timer) >= rd<float>(B.duration)) return;
    if (rd<u8>(B.success) || rd<u8>(B.failed)) return;
    s32 prompt = rd<s32>(B.minigame);
    u32 faces = pressed & 15;  // A B X Y = green red blue yellow
    u32 want = 1u << (prompt & 3);
    u32 want_dir = 1u << (prompt & 3);  // left right up down
    if (prompt >= 0 && prompt <= 3) {
        if (faces) {
            bool ok = faces == want;
            wr<u8>(B.success, ok);
            wr<u8>(B.failed, !ok);
        }
    } else if (prompt >= 4 && prompt <= 7) {
        if (new_dirs) {
            bool ok = new_dirs == want_dir;
            wr<u8>(B.success, ok);
            wr<u8>(B.failed, !ok);
        }
    } else if (prompt >= 8 && prompt <= 11) {
        if (faces & want) add_progress(rd<float>(B.tuning) * 0.2001f);
    } else if (prompt >= 12 && prompt <= 15) {
        if (faces & want) judge_timing();
    } else if (prompt >= 16 && prompt <= 19) {
        if (new_dirs & want_dir) judge_timing();
    } else if (prompt == 20) {
        float x = axis(0), y = axis(1);
        if (g_rub_valid) {
            float dx = x - g_rub_x, dy = y - g_rub_y;
            float window = rd<float>(B.duration) - 0.2f;
            if (window < 0.2f) window = 0.2f;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > 0) add_progress(d * rd<float>(B.tuning) * 0.12f / (window * 0.85f));
        }
        g_rub_x = x, g_rub_y = y, g_rub_valid = true;
        return;
    }
    if (prompt != 20) g_rub_valid = false;
}

// Debug (files/padauto): feeds the controller input the active prompt asks for, through the same
// paths as a real controller: face button presses, d-pad flicks, stick wiggles.
void auto_play(u32 &pressed, u32 &buttons) {
    static int frame;
    frame++;
    s32 prompt = rd<s32>(B.minigame);
    bool in_zone = rd<s32>(B.bar) >= 0x47 && rd<s32>(B.bar) <= 0x50;
    static const u32 dpad[4] = {1u << BTN_DPLEFT, 1u << BTN_DPRIGHT, 1u << BTN_DPUP, 1u << BTN_DPDOWN};
    g_debug_stick.store(false);
    if (prompt >= 0 && prompt <= 3) {
        if (frame % 20 == 0) pressed |= 1u << prompt;
    } else if (prompt >= 4 && prompt <= 7) {
        if (frame % 20 < 3) buttons |= dpad[prompt - 4];
    } else if (prompt >= 8 && prompt <= 11) {
        if (frame % 2 == 0) pressed |= 1u << (prompt - 8);
    } else if (prompt >= 12 && prompt <= 15) {
        if (in_zone) pressed |= 1u << (prompt - 12);
    } else if (prompt >= 16 && prompt <= 19) {
        if (in_zone) buttons |= dpad[prompt - 16];
    } else if (prompt == 20) {
        g_debug_stick.store(true);
        g_debug_lx.store(frame % 2 ? 30000 : -30000);
        g_debug_ly.store(frame % 4 < 2 ? 30000 : -30000);
    }
}

// void Battle_PrepareMiniGame(BattlePrompt)
void t_prepare(s32 prompt) {
    int forced = g_force_prompt.load(std::memory_order_relaxed);
    cpu::call32(g_prepare_orig, {(u32)(forced >= 0 ? forced : prompt)});
}

// bool Battle_HandleInput(const VirtualGamepad &): true while a battle takes the input (the
// player controller then ignores it, e.g. for advancing speech bubbles), so the result must be
// passed through.
u32 t_handle_input(u32 vpad) {
    u32 handled = cpu::call32(g_handle_input_orig, {vpad}) & 0xff;  // touch, menus, targeting
    u32 pressed = g_pressed.exchange(0, std::memory_order_relaxed);
    u32 buttons = g_buttons.load(std::memory_order_relaxed) | g_debug_buttons.load(std::memory_order_relaxed);
    static s32 logged_prompt = -1;
    bool active = rd<s32>(B.state) != -1 && rd<s32>(B.input_state) == 6;
    if (active && g_auto.load(std::memory_order_relaxed)) {
        pressed = 0, buttons = 0;  // only the synthetic input counts
        auto_play(pressed, buttons);
    }
    u32 dirs = held_dirs(buttons, g_prev_dirs);
    u32 new_dirs = dirs & ~g_prev_dirs;
    g_prev_dirs = dirs;
    if (!active) {
        g_rub_valid = false;
        if (logged_prompt >= 0) {
            LOGI("gamepad: prompt %d ended: success=%u failed=%u progress=%.2f", logged_prompt,
                 rd<u8>(B.success), rd<u8>(B.failed), rd<float>(B.progress));
            logged_prompt = -1;
        }
        return handled;
    }
    if (logged_prompt != rd<s32>(B.minigame)) {
        logged_prompt = rd<s32>(B.minigame);
        LOGI("gamepad: prompt %d started (tuning %.2f, duration %.2f)", logged_prompt, rd<float>(B.tuning),
             rd<float>(B.duration));
    }
    pad_prompt(pressed, new_dirs);
    if (rd<u8>(B.success) || rd<u8>(B.failed)) wr<s32>(B.input_state, 1);
    return handled;
}

// ---- button icons
//
// datapatch adds the Xbox icons to Mobile_InputIcons.gfx, the library every menu imports its
// button icons from, with their images renamed "X360_*". Images resolve to textures named
// "UI/Flash/<library>/<image>"; the X360 ones are mapped to Xbox360_ControllerIcons' textures, or
// to PS3_ControllerIcons' while the controller in use is a PlayStation one
// (DevTools.nativePadIcons). A few movies carry private copies of Xbox icons (the message box's
// A/B, the pause screen's stick icons); those are redirected to the PS3 ones too.
// The icon library is loaded once and stays loaded (the battle HUD, for one, keeps its icons for
// the whole game), so every icon image created is remembered, and when the controller type
// changes the game thread points each one at the other set's texture (GImageInfo::SetTexture).

std::atomic<int> g_icon_set{0};  // 0: Xbox, 1: PlayStation
u32 g_create_platform_texture;  // FlashManager::CreatePlatformTexture(Texture *)
u32 g_flash_manager;            // &g_pFlashManager
u32 g_set_texture;              // GImageInfo::SetTexture(GTexture *, unsigned, unsigned)
u32 g_image_d0_orig, g_image_d1_orig;  // GImageInfo destructors
std::mutex g_images_mutex;
std::unordered_map<u32, std::string> g_images;  // live icon GImageInfo -> icon key (see icon_key)
thread_local std::string t_bound_icon;          // icon key of the texture CreateImage just bound

bool starts_with_ci(const char *s, const char *prefix) { return strncasecmp(s, prefix, strlen(prefix)) == 0; }

// Xbox icon textures (in Xbox360_ControllerIcons unless a folder is given) whose PS3 equivalent
// has another name. The unnamed ones are the stick icons, matched by picture: R, L, then each
// stick pushed in a direction.
constexpr struct { const char *xbox, *ps3; } PS3_NAMES[] = {
    {"BUTTON_Aorig", "BUTTON_A"},
    {"MessageBox/MessageBox_IA", "BUTTON_A"},
    {"MessageBox/MessageBox_I7", "BUTTON_B"},
    {"xbox360_controllericons_I16", "PS3_ControllerIcons_I14"},  // R
    {"xbox360_controllericons_I17", "PS3_ControllerIcons_I15"},  // L
    {"xbox360_controllericons_I18", "PS3_ControllerIcons_I16"},  // R up
    {"xbox360_controllericons_I19", "PS3_ControllerIcons_I17"},  // R down
    {"xbox360_controllericons_I1A", "PS3_ControllerIcons_I19"},  // L down
    {"xbox360_controllericons_I1B", "PS3_ControllerIcons_I18"},  // L up
    {"xbox360_controllericons_I1C", "PS3_ControllerIcons_I1B"},  // R right
    {"xbox360_controllericons_I1D", "PS3_ControllerIcons_I1C"},  // R left
    {"xbox360_controllericons_I1E", "PS3_ControllerIcons_I1A"},  // L right
    {"xbox360_controllericons_I1F", "PS3_ControllerIcons_I1D"},  // L left
    {"Pause/Pause_I2B", "PS3_ControllerIcons_I16"},
    {"Pause/Pause_I34", "PS3_ControllerIcons_I17"},
    {"Pause/Pause_I49", "PS3_ControllerIcons_I19"},
    {"Pause/Pause_I40", "PS3_ControllerIcons_I18"},
    {"Pause/Pause_I2E", "PS3_ControllerIcons_I1B"},
    {"Pause/Pause_I31", "PS3_ControllerIcons_I1C"},
    {"Pause/Pause_I43", "PS3_ControllerIcons_I1A"},
    {"Pause/Pause_I46", "PS3_ControllerIcons_I1D"},
};

// The PS3 texture for an Xbox icon file name ("BUTTON_Y.dds", or "<folder>/<file>" for the
// private copies), or "" if it has none.
std::string ps3_texture(const char *name) {
    const char *ext = strrchr(name, '.');
    size_t len = ext ? ext - name : strlen(name);
    for (const auto &m : PS3_NAMES)
        if (strlen(m.xbox) == len && strncasecmp(name, m.xbox, len) == 0)
            return std::string("UI/Flash/PS3_ControllerIcons/") + m.ps3 + (ext ? ext : "");
    return strchr(name, '/') ? "" : std::string("UI/Flash/PS3_ControllerIcons/") + name;
}

// Icon key of a texture path: "X360_<file>" for the library's Xbox images, "<folder>/<file>" for
// the private copies, "" for anything else.
std::string icon_key(const char *path) {
    if (!path || !starts_with_ci(path, "UI/Flash/")) return "";
    const char *rel = path + 9, *slash = strchr(rel, '/');
    if (!slash) return "";
    if (starts_with_ci(slash + 1, "X360_")) return slash + 1;
    return ps3_texture(rel).empty() ? "" : rel;
}

// Texture path of an icon in an icon set.
std::string icon_path(const std::string &key, int set) {
    if (starts_with_ci(key.c_str(), "X360_")) {
        const char *name = key.c_str() + 5;
        return set == 1 ? ps3_texture(name) : std::string("UI/Flash/Xbox360_ControllerIcons/") + name;
    }
    return set == 1 ? ps3_texture(key.c_str()) : "UI/Flash/" + key;
}

// Guest copy of a string, kept for good (texture paths; a few dozen).
u32 guest_string(const std::string &s) {
    static std::unordered_map<std::string, u32> cache;
    u32 &g = cache[s];
    if (!g) {
        g = mem::halloc(s.size() + 1, 4);
        memcpy(g2h(g), s.c_str(), s.size() + 1);
    }
    return g;
}

// RsRef<Texture> RsRef<Texture>::BindFile(const char *path, bool)
u32 t_bind_texture(u32 path, u32 flag) {
    std::string key = icon_key((const char *)g2h(path));
    if (!key.empty()) {
        std::string mapped = icon_path(key, g_icon_set.load(std::memory_order_relaxed));
        if (strcasecmp(mapped.c_str(), (const char *)g2h(path)) != 0) {
            static std::unordered_map<std::string, bool> logged;
            if (!logged[mapped]) logged[mapped] = true, LOGI("gamepad: icon texture %s -> %s", (const char *)g2h(path), mapped.c_str());
            path = guest_string(mapped);
        }
    }
    t_bound_icon = std::move(key);
    return cpu::call32(g_bind_texture_orig, {path, flag});
}

// The loaded Texture for a path, labelled with the size its image declares: images get their
// texture's size, but Xbox360_ControllerIcons' A is a 64x64 texture where the menus expect a
// 32x32 icon (the Xbox library drew it through a half-size sprite), and GFx maps fills to the
// texture by the texture's own size, so the (already uploaded) Texture is relabelled; this also
// fits the PS3 textures into the Xbox icons' sizes. Each path is bound once; its reference is
// kept, so the texture stays loaded.
u32 icon_texture(const std::string &path, u32 w, u32 h) {
    static std::unordered_map<std::string, u32> refs;  // path -> guest RsRef<Texture>
    u32 &ref = refs[path];
    if (!ref) {
        ref = mem::halloc(4, 4);
        wr<u32>(ref, cpu::call32(g_bind_texture_orig, {guest_string(path), 1}));
    }
    u32 tex = cpu::call32(g_texture_lock, {ref});
    if (tex && (rd<u16>(tex + 0x24) != w || rd<u16>(tex + 0x26) != h)) {
        LOGI("gamepad: icon %s: %ux%u texture drawn as %ux%u", path.c_str(), rd<u16>(tex + 0x24), rd<u16>(tex + 0x26), w, h);
        wr<u16>(tex + 0x24, w), wr<u16>(tex + 0x26, h);
    }
    return tex;
}

// GImageInfo *FlashImageCreator::CreateImage(const GFxImageCreateInfo &)
// For icon images, uses the size the movie declares (see icon_texture) and remembers the image.
u32 t_create_image(u32 self, u32 info) {
    t_bound_icon.clear();
    u32 image = cpu::call32(g_create_image_orig, {self, info});
    std::string key = std::move(t_bound_icon);
    u32 file_info = rd<u32>(info + 0xc);
    if (!image || key.empty() || rd<u32>(info) != 2 || !file_info) return image;
    u16 w = rd<u16>(file_info + 0x1c), h = rd<u16>(file_info + 0x1e);  // declared size
    if (!w || !h) return image;
    icon_texture(icon_path(key, g_icon_set.load(std::memory_order_relaxed)), w, h);
    wr<u32>(image + 0x1c, w), wr<u32>(image + 0x20, h);  // GImageInfo Width, Height
    std::lock_guard<std::mutex> lock(g_images_mutex);
    g_images[image] = std::move(key);
    return image;
}

// GImageInfo::~GImageInfo (complete and deleting): forget the image.
u32 t_image_d1(u32 self) {
    { std::lock_guard<std::mutex> lock(g_images_mutex); g_images.erase(self); }
    return cpu::call32(g_image_d1_orig, {self});
}
u32 t_image_d0(u32 self) {
    { std::lock_guard<std::mutex> lock(g_images_mutex); g_images.erase(self); }
    return cpu::call32(g_image_d0_orig, {self});
}

// Points every live icon image at the current icon set's texture (game thread).
void switch_icons() {
    int set = g_icon_set.load(std::memory_order_relaxed);
    u32 manager = rd<u32>(g_flash_manager);
    if (!manager) return;
    std::lock_guard<std::mutex> lock(g_images_mutex);
    for (const auto &[image, key] : g_images) {
        u32 w = rd<u32>(image + 0x1c), h = rd<u32>(image + 0x20);
        u32 tex = icon_texture(icon_path(key, set), w, h);
        u32 gtex = tex ? cpu::call32(g_create_platform_texture, {manager, tex}) : 0;
        if (!gtex) continue;
        cpu::call32(g_set_texture, {image, gtex, w, h});  // takes its own reference
        u32 refs = rd<u32>(gtex + 4);
        wr<u32>(gtex + 4, refs - 1);
        if (refs == 1) cpu::call32(rd<u32>(rd<u32>(gtex) + 4), {gtex});  // GRefCountBase release
    }
    LOGI("gamepad: %zu icon images switched to %s", g_images.size(), set == 1 ? "PlayStation" : "Xbox");
}

// void Battle_PushFlashVars(FlashValueSnapshot *)
void t_push_vars(u32 snapshot) {
    cpu::call32(g_push_vars_orig, {snapshot});
    u32 show_pc = rd<u8>(B.is_gamepad) == 0;
    static u32 logged = 2;
    if (show_pc != logged) LOGI("gamepad: battle HUD ShowPCButtons=%u", logged = show_pc);
    cpu::call32(g_set_bool, {snapshot, g_name_show_pc, show_pc});
    // The HUD script sets the ability button (button_y) to the costume's touch picture only when
    // the costume changes, but puts it on its controller frame (the Y icon) at every battle start
    // and on every switch to controller icons. So from the second battle with the same costume,
    // and after any switch back to touch, it showed Y. Forgetting the costume it last showed makes
    // it pick the right frame on every update: "Robot" != false, while no costume ("") == false.
    cpu::call32(g_set_bool, {snapshot, g_name_last_costume, 0});
    // The tap targets around a prompt are for touch; with a controller the prompt text (with its
    // button icon) is all the PC version shows.
    if (!show_pc) cpu::call32(g_set_bool, {snapshot, g_name_minigame_visible, 0});
}

void j_pad_icons(JNIEnv *, jclass, jint set) {
    if (g_icon_set.exchange(set == 1, std::memory_order_relaxed) != (set == 1)) {
        LOGI("gamepad: button icons: %s", set == 1 ? "PlayStation" : "Xbox");
        g_icons_changed.store(true);
    }
}

void j_pad(JNIEnv *, jclass, jint buttons, jint lx, jint ly, jint rx, jint ry, jint lt, jint rt) {
    const jint v[NAXES] = {lx, ly, rx, ry, lt, rt};
    for (int i = 0; i < NAXES; i++) g_axes[i].store(v[i], std::memory_order_relaxed);
    u32 old = g_buttons.exchange((u32)buttons, std::memory_order_relaxed);
    g_pressed.fetch_or((u32)buttons & ~old, std::memory_order_relaxed);
}
}  // namespace

void force_prompt(int prompt) { g_force_prompt.store(prompt); }
void set_auto(bool on) { g_auto.store(on); if (!on) g_debug_stick.store(false); }

void debug_press(u32 mask, int ms) {
    g_debug_buttons.fetch_or(mask);
    g_pressed.fetch_or(mask);
    usleep(ms * 1000);
    g_debug_buttons.fetch_and(~mask);
}

void install(Module *cq) {
    g_private_axis = loader::lookup_in(cq, "SDL_PrivateJoystickAxis");
    g_private_button = loader::lookup_in(cq, "SDL_PrivateJoystickButton");
    if (!g_private_axis || !g_private_button) fatal("gamepad: SDL_PrivateJoystick* not found");
    redirect(cq, "SDL_SYS_JoystickOpen", cpu::add_thunk("pad_open", &abi::wrapped<t_open>));
    redirect(cq, "SDL_SYS_JoystickUpdate", cpu::add_thunk("pad_update", &abi::wrapped<t_update>));
    redirect(cq, "SDL_IsGameController", cpu::add_thunk("pad_is_gc", &abi::wrapped<t_is_gc>));
    auto need = [&](const char *name) {
        u32 a = loader::lookup_in(cq, name);
        if (!a) fatal("gamepad: %s not found", name);
        return a;
    };
    B.state = need("g_BattleState");
    B.input_state = need("g_BattleInputState");
    B.minigame = need("g_BattleMiniGame");
    B.tuning = need("g_BattleMiniGameTuning");
    B.timer = need("g_BattleMiniGameTimer");
    B.duration = need("g_BattleMiniGameDuration");
    B.progress = need("g_BattleMiniGameProgress");
    B.success = need("g_BattleMiniGameSuccess");
    B.failed = need("g_BattleMiniGameFailed");
    B.bar = need("g_BattleProgressBar");
    B.bar_adjusted = need("g_BattleProgressBarAdjusted");
    B.is_gamepad = need("_ZN21VirtualGamepadManager12m_bIsGamepadE");
    g_set_bool = need("_ZN18FlashValueSnapshot17SetGlobalVariableERK4Nameb");
    // Name::_LookupString(const char *, unsigned, ConstructorTag) returns the interned handle.
    auto name = [&](const char *text) {
        size_t n = strlen(text) + 1;
        u32 str = mem::halloc(n, 4), slot = mem::halloc(4, 4);
        memcpy(g2h(str), text, n);
        wr<u32>(slot, cpu::call32(need("_ZN4Name13_LookupStringEPKcjNS_14ConstructorTagE"), {str, (u32)n - 1, 2}));
        return slot;
    };
    g_name_show_pc = name("ShowPCButtons");
    g_name_minigame_visible = name("MinigameVisible");
    g_name_last_costume = name("_lastCurrentCostume");
    if (datapatch::icon_library_ready()) {
        g_bind_texture_orig = hook_arm(cq, "_ZN5RsRefI7TextureE8BindFileEPKcb",
                                       cpu::add_thunk("pad_bind_texture", &abi::wrapped<t_bind_texture>));
        g_texture_lock = loader::lookup_in(cq, "_ZNK5RsRefI7TextureE4LockEv");
        g_create_image_orig = hook_arm(cq, "_ZN17FlashImageCreator11CreateImageERK18GFxImageCreateInfo",
                                       cpu::add_thunk("pad_create_image", &abi::wrapped<t_create_image>));
        g_image_d0_orig = hook_arm(cq, "_ZN10GImageInfoD0Ev", cpu::add_thunk("pad_image_d0", &abi::wrapped<t_image_d0>));
        g_image_d1_orig = hook_arm(cq, "_ZN10GImageInfoD1Ev", cpu::add_thunk("pad_image_d1", &abi::wrapped<t_image_d1>));
        g_create_platform_texture = need("_ZN12FlashManager21CreatePlatformTextureEP7Texture");
        g_flash_manager = need("g_pFlashManager");
        g_set_texture = need("_ZN10GImageInfo10SetTextureEP8GTexturejj");
        LOGI("gamepad: button icons: mobile + Xbox/PlayStation library");
    }
    g_prepare_orig = hook_arm(cq, "_Z22Battle_PrepareMiniGame12BattlePrompt",
                              cpu::add_thunk("pad_prepare", &abi::wrapped<t_prepare>));
    g_handle_input_orig = hook_arm(cq, "_Z18Battle_HandleInputRK14VirtualGamepad",
                                   cpu::add_thunk("pad_battle_input", &abi::wrapped<t_handle_input>));
    g_push_vars_orig = hook_arm(cq, "_Z20Battle_PushFlashVarsP18FlashValueSnapshot",
                                cpu::add_thunk("pad_battle_vars", &abi::wrapped<t_push_vars>));
    setenv("SDL_GAMECONTROLLERCONFIG", MAPPING, 1);
}

void register_natives(JNIEnv *env) {
    jclass c = env->FindClass("com/doublefine/cq/DevTools");
    if (!c) {
        env->ExceptionClear();
        return;
    }
    static const JNINativeMethod m[] = {
        {"nativePad", "(IIIIIII)V", (void *)&j_pad},
        {"nativePadIcons", "(I)V", (void *)&j_pad_icons},
    };
    if (env->RegisterNatives(c, m, 2) != JNI_OK) fatal("RegisterNatives DevTools gamepad natives failed");
}
}  // namespace gamepad
