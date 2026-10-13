// SPDX-License-Identifier: GPL-3.0-or-later
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
// Battle prompts are touch-only on Android: the coloured icons (green/red/blue/yellow, the
// console's A/B/X/Y) must be tapped, at positions shuffled per prompt. DevTools turns a face
// button press during a prompt into a touch, and Battle_GetButtonOverlap (which judges whether
// a tap hit the prompted colour) is hooked to judge such touches by the button's colour instead
// of where they landed.
#include "gamepad.h"

#include "abi.h"
#include "loader.h"

#include <atomic>
#include <cstdlib>
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
u32 g_battle_state, g_battle_minigame;  // &g_BattleState, &g_BattleMiniGame
u32 g_overlap_orig;                     // trampoline to the original Battle_GetButtonOverlap
std::atomic<int> g_tap_colour{-1};      // colour of the controller button behind the current touch
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
void t_update(u32 joy) {
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

// bool Battle_GetButtonOverlap(BattlePrompt prompt, const vec2 &pos)
// Prompts 0-3 / 8-11 / 12-15 are Press / Mash / Meter Green, Red, Blue, Yellow.
u32 t_overlap(s32 prompt, u32 pos) {
    int colour = g_tap_colour.load(std::memory_order_relaxed);
    if (colour >= 0 && ((prompt >= 0 && prompt <= 3) || (prompt >= 8 && prompt <= 15)))
        return (prompt & 3) == colour;
    return cpu::call32(g_overlap_orig, {(u32)prompt, pos}) & 0xff;
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

// The active battle prompt (BattlePrompt), or -1.
jint j_battle_prompt(JNIEnv *, jclass) {
    if (rd<s32>(g_battle_state) != 5) return -1;  // 5: a prompt is up
    return rd<s32>(g_battle_minigame);
}
void j_tap_colour(JNIEnv *, jclass, jint colour) { g_tap_colour.store(colour, std::memory_order_relaxed); }

void j_pad(JNIEnv *, jclass, jint buttons, jint lx, jint ly, jint rx, jint ry, jint lt, jint rt) {
    const jint v[NAXES] = {lx, ly, rx, ry, lt, rt};
    for (int i = 0; i < NAXES; i++) g_axes[i].store(v[i], std::memory_order_relaxed);
    g_buttons.store((u32)buttons, std::memory_order_relaxed);
}
}  // namespace

void debug_press(u32 mask, int ms) {
    g_debug_buttons.fetch_or(mask);
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
    g_battle_state = loader::lookup_in(cq, "g_BattleState");
    g_battle_minigame = loader::lookup_in(cq, "g_BattleMiniGame");
    if (!g_battle_state || !g_battle_minigame) fatal("gamepad: battle globals not found");
    g_overlap_orig = hook_arm(cq, "_Z23Battle_GetButtonOverlap12BattlePromptRK4vec2",
                              cpu::add_thunk("pad_overlap", &abi::wrapped<t_overlap>));
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
        {"nativeBattlePrompt", "()I", (void *)&j_battle_prompt},
        {"nativeTapColour", "(I)V", (void *)&j_tap_colour},
    };
    if (env->RegisterNatives(c, m, 3) != JNI_OK) fatal("RegisterNatives DevTools gamepad natives failed");
}
}  // namespace gamepad
