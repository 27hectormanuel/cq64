// SPDX-License-Identifier: CC0-1.0
#pragma once
// Java native methods forwarded into the guest. Each game profile defines g_natives[] with the N()
// macro below; host_main looks each one up in the guest as Java_<class>_<method> and registers a
// typed trampoline that marshals the arguments into AAPCS (softfp) words and calls it.
#include "abi.h"
#include "jni_bridge.h"

#include <jni.h>

#include <cstring>
#include <type_traits>
#include <vector>

struct NativeDef {
    const char *cls, *name, *sig;
    void *fn;
    u32 guest = 0;
};
extern NativeDef g_natives[];
extern const size_t g_native_count;

namespace natives {
struct Packer {
    std::vector<u32> w;
    void word(u32 v) { w.push_back(v); }
    void dword(u64 v) {
        if (w.size() & 1) w.push_back(0);  // AAPCS: 64-bit values start at an even slot
        w.push_back((u32)v);
        w.push_back((u32)(v >> 32));
    }
};
inline void pack(Packer &p, jint v) { p.word((u32)v); }
inline void pack(Packer &p, jboolean v) { p.word(v); }
inline void pack(Packer &p, jfloat v) { u32 x; memcpy(&x, &v, 4); p.word(x); }
inline void pack(Packer &p, jlong v) { p.dword((u64)v); }
inline void pack(Packer &p, jdouble v) { u64 x; memcpy(&x, &v, 8); p.dword(x); }
inline void pack(Packer &p, jobject v) { p.word(jni::to_guest(v)); }

template <int I, class R, class... A> R tramp(JNIEnv *env, jobject self, A... a) {
    jni::NativeFrame frame;
    Packer p;
    p.word(jni::guest_env());
    p.word(jni::to_guest(self));
    (pack(p, a), ...);
    u64 r = cpu::call(g_natives[I].guest, p.w.data(), p.w.size());
    if constexpr (std::is_void_v<R>) {
        (void)r;
        return;
    } else if constexpr (std::is_same_v<R, jobject>) {
        return env->NewLocalRef(jni::to_host((u32)r));
    } else if constexpr (std::is_same_v<R, jlong>) {
        return (jlong)r;
    } else if constexpr (std::is_same_v<R, jdouble>) {
        double d;
        memcpy(&d, &r, 8);  // softfp: doubles come back in r0:r1
        return d;
    } else if constexpr (std::is_same_v<R, jfloat>) {
        u32 w = (u32)r;
        float f;
        memcpy(&f, &w, 4);
        return f;
    } else {
        return (R)(u32)r;
    }
}
}  // namespace natives

using O = jobject;
#define N(i, cls, name, sig, ...) {cls, name, sig, (void *)&natives::tramp<i, __VA_ARGS__>}
