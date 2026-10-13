// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

#include <array>
#include <memory>
#include <vector>
#include <initializer_list>

namespace Dynarmic::A32 { class Jit; }

struct GuestThread;
using ThunkFn = void (*)(GuestThread &);

// Thrown through host frames to unwind a guest thread (pthread_exit).
struct GuestThreadExit { u32 value; };

struct GuestThread {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    struct Callbacks;
    std::unique_ptr<Callbacks> cb;

    u32 stack_base = 0, stack_size = 0;
    u32 errno_addr = 0;     // per-thread errno slot in guest memory
    u32 handle = 0;         // guest-visible pthread_t
    u32 tls_block = 0;      // TPIDRURO target
    int processor_id = 0;
    int depth = 0;          // nesting of host->guest calls
    u32 cur_svc = 0;        // id of the thunk being dispatched
    bool pc_changed = false;  // set by thunks that redirect control flow
    std::vector<u32> tls_values;  // pthread_key values

    u32 &r(int i);
    u32 sp();
    u32 cpsr();
    void set_errno(int e) { wr<s32>(errno_addr, e); }

    // AAPCS argument access (softfp: all args in r0-r3 then stack).
    u32 arg(int i) { return i < 4 ? r(i) : rd<u32>(sp() + 4 * (i - 4)); }
    void ret32(u32 v) { r(0) = v; }
    void ret64(u64 v) { r(0) = (u32)v; r(1) = (u32)(v >> 32); }
};

GuestThread &cur_thread();            // creates a GuestThread for the calling host thread if needed
GuestThread *cur_thread_or_null();
void destroy_cur_thread();
// Must be called on a new host thread before cur_thread() to give it a preallocated pthread_t.
void cpu_assign_handle(u32 h);

namespace cpu {
void init();
// Calls guest function `fn` (bit 0 = Thumb) with up to N word arguments; returns r0 | r1 << 32.
u64 call(u32 fn, const u32 *args, size_t n);
inline u64 call(u32 fn, std::initializer_list<u32> args) { return call(fn, args.begin(), args.size()); }
inline u32 call32(u32 fn, std::initializer_list<u32> args) { return (u32)call(fn, args); }

// Thunk registry: each thunk gets a guest address containing "svc #id".
u32 add_thunk(const char *name, ThunkFn fn);
const char *thunk_name(u32 id);
// Fixed thunk ids
constexpr u32 SVC_RETURN = 0;  // return from host->guest call

void dump_state(GuestThread &t);
// Makes every running guest thread log its state at its next block boundary.
void request_dump_all();
}  // namespace cpu
