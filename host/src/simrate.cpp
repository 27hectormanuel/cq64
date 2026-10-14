// SPDX-License-Identifier: CC0-1.0
// High frame rate: runs Costume Quest's simulation at 60 Hz instead of 30.
//
// The engine renders as fast as the display allows, but the world (movement, animation, battles)
// only advances in fixed simulation ticks of SimWorld::kfDefaultSimulationPeriod = 1/30 s, so
// the game looks like 30 fps outside the menus. Costume Quest 2 (same engine, later version)
// added a "60 FPS" option, GameApp::SetSim60Hz: its Linux build ships with symbols, which show
// what it changes; the same is done here to Costume Quest's engine:
//  - the default period (SimWorld::sm_fSimulationPeriod in CQ2, the constant
//    kfDefaultSimulationPeriod here), read by GameApp::Init (a smoothing factor,
//    powf(0.1, period)) and GameApp::_RequestRender (render pacing): set to 1/60 before the game
//    initializes;
//  - each SimWorld's tick period / frequency (+0xa40 / +0xa44, 1/30 and 30 in its constructor):
//    the constructor's constants are patched;
//  - SimWorld::_UpdateTimeScale recomputes them for slow motion from hard-coded 1/30 and 30
//    (CQ2 reads its globals): hooked to recompute them at the new rate.
// Opt-in (files/simrate holds the rate, written by the developer menu), applied at startup.
//
// Worker threads: the engine spreads simulation tasks (animation, particles, visibility) and
// scene graph work over thread pools. Costume Quest 2 on PC gives them 2 worker threads each;
// this Android build creates the simulation pool with none and the scene graph's with one, so
// nearly everything runs on the game thread (one core). With files/workers, both get 2.
#include "simrate.h"

#include "abi.h"
#include "game.h"
#include "loader.h"

#include <cstdio>
#include <unistd.h>

namespace simrate {
namespace {
constexpr u32 PERIOD = 0xa40, FREQUENCY = 0xa44, TIME_SCALE = 0xa50;  // SimWorld fields
float g_hz;
u32 g_update_time_scale_orig;

// Replaces the ARM function `fn` with "ldr pc, [pc, #-4]; .word thunk"; returns a trampoline
// running its first two (position-independent) instructions, then the rest.
u32 hook_arm(u32 fn, u32 thunk) {
    u32 tramp = mem::halloc(16, 4);
    wr<u32>(tramp, rd<u32>(fn));
    wr<u32>(tramp + 4, rd<u32>(fn + 4));
    wr<u32>(tramp + 8, 0xE51FF004);  // ldr pc, [pc, #-4]
    wr<u32>(tramp + 12, fn + 8);
    wr<u32>(fn, 0xE51FF004);
    wr<u32>(fn + 4, thunk);
    return tramp;
}

// void SimWorld::_UpdateTimeScale()
void t_update_time_scale(u32 world) {
    cpu::call32(g_update_time_scale_orig, {world});
    float scale = rd<float>(world + TIME_SCALE);
    if (scale <= 0) scale = 1;
    wr<float>(world + PERIOD, scale / g_hz);
    wr<float>(world + FREQUENCY, g_hz / scale);
}

// "movt rd, #imm16" with a new immediate.
u32 movt(u32 insn, u16 imm) { return (insn & 0xfff0f000) | ((imm >> 12) << 16) | (imm & 0xfff); }

// In the first instructions of `fn`, changes "movt rX, #from" to "movt rX, #to".
bool patch_movt(u32 fn, u16 from, u16 to) {
    for (u32 a = fn; a < fn + 0x200; a += 4) {
        u32 x = rd<u32>(a);
        if ((x & 0x0ff00000) == 0x03400000 && ((x >> 4) & 0xf000 | (x & 0xfff)) == from) {
            wr<u32>(a, movt(x, to));
            return true;
        }
    }
    return false;
}
// "mov r1, #from" at fn + off becomes "mov r1, #to" (the ThreadPool worker count argument).
bool patch_mov_r1(u32 fn, u32 off, u8 from, u8 to) {
    if (rd<u32>(fn + off) != (0xe3a01000u | from)) return false;
    wr<u32>(fn + off, 0xe3a01000u | to);
    return true;
}

void enable_workers(Module *cq) {
    if (access((game::files_dir() + "/workers").c_str(), F_OK) != 0) return;
    u32 sim = loader::lookup_in(cq, "_ZN8SimWorld27_InitializeDefaultThreadingEb") & ~1u;
    u32 scene = loader::lookup_in(cq, "_ZN10SceneGraphC2Ev") & ~1u;
    // Both branches of SimWorld::_InitializeDefaultThreading pass 0 threads; SceneGraph's pool 1.
    bool ok = sim && scene && patch_mov_r1(sim, 0x64, 0, 2) && patch_mov_r1(sim, 0x84, 0, 2) &&
              patch_mov_r1(scene, 0x768, 1, 2);
    LOGI(ok ? "simrate: 2 worker threads for simulation and scene graph tasks"
            : "simrate: thread pool setup not as expected; workers unchanged");
}
}  // namespace

int configured_rate() {
    int hz = 0;
    if (FILE *f = fopen((game::files_dir() + "/simrate").c_str(), "r")) {
        if (fscanf(f, "%d", &hz) != 1) hz = 0;
        fclose(f);
    }
    return hz == 60 || hz == 120 ? hz : 30;
}

void install(Module *cq) {
    enable_workers(cq);
    int hz = configured_rate();
    if (hz == 30) return;
    u32 period = loader::lookup_in(cq, "_ZN8SimWorld25kfDefaultSimulationPeriodE");
    u32 ctor = loader::lookup_in(cq, "_ZN8SimWorldC2E9WorldTypeP12PhysicsWorld") & ~1u;
    u32 update = loader::lookup_in(cq, "_ZN8SimWorld16_UpdateTimeScaleEv") & ~1u;
    if (!period || !ctor || !update || rd<float>(period) != 1.0f / 30) {
        LOGW("simrate: not this engine version; staying at 30 Hz");
        return;
    }
    // The constructor builds 1/30 (0x3d088889) and 30.0 (0x41f00000) with movw/movt; the low
    // halves (0x8889, 0) are the same for 1/60 and 60.0, and close enough for 1/120 and 120.
    u16 period_hi = hz == 60 ? 0x3c88 : 0x3c08, freq_hi = hz == 60 ? 0x4270 : 0x42f0;
    if (!patch_movt(ctor, 0x3d08, period_hi) || !patch_movt(ctor, 0x41f0, freq_hi)) {
        LOGW("simrate: SimWorld constructor not as expected; staying at 30 Hz");
        return;
    }
    g_hz = (float)hz;
    wr<float>(period, 1.0f / hz);
    g_update_time_scale_orig = hook_arm(update, cpu::add_thunk("simrate_time_scale", &abi::wrapped<t_update_time_scale>));
    LOGI("simrate: simulation at %d Hz", hz);
}
}  // namespace simrate
