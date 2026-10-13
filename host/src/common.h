// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Shared definitions for the 64-bit host runtime that runs the 32-bit ARM game code.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <android/log.h>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

#define LOG_TAG "cqhost"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- guest address space ----
// The whole 32-bit guest address space is one 4 GiB host mapping starting at g_base,
// so guest address A is host address g_base + A.
extern u8 *g_base;

inline void *g2h(u32 a) { return a ? g_base + a : nullptr; }
inline u32 h2g(const void *p) {
    if (!p) return 0;
    return (u32)((const u8 *)p - g_base);
}
inline bool is_guest_ptr(const void *p) { return (const u8 *)p >= g_base && (const u8 *)p < g_base + 0x100000000ull; }

template <class T> inline T rd(u32 a) { T v; memcpy(&v, g_base + a, sizeof v); return v; }
template <class T> inline void wr(u32 a, T v) { memcpy(g_base + a, &v, sizeof v); }

// Typed guest pointer used in thunk signatures: converts to a host pointer.
template <class T> struct G {
    u32 a;
    operator T *() const { return (T *)g2h(a); }
    T *get() const { return (T *)g2h(a); }
    explicit operator bool() const { return a != 0; }
};

namespace mem {
void init();
// Page-granular allocation of guest address space (used for mmap, stacks, module images).
u32 map(u32 size, u32 hint = 0);
void unmap(u32 addr, u32 size);
// Small allocations owned by the host (FILE structs, strings returned to the guest, ...).
u32 halloc(u32 size, u32 align = 8);
void hfree(u32 addr);
u32 hstrdup(const char *s);
}  // namespace mem

// Fixed guest layout
constexpr u32 GUEST_THUNK_BASE = 0x00010000;  // one 4-byte "svc #id" per thunk
constexpr u32 GUEST_THUNK_MAX = 0x4000;
constexpr u32 GUEST_HHEAP_BASE = 0x00100000;  // host-owned small object heap
constexpr u32 GUEST_HHEAP_SIZE = 0x07F00000;
constexpr u32 GUEST_MAP_BASE = 0x08000000;    // mmap/module/stack region
constexpr u32 GUEST_MAP_END = 0xFFF00000;
