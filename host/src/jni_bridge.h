// SPDX-License-Identifier: CC0-1.0
#pragma once
#include "common.h"

#include <jni.h>

namespace jni {
void init(JavaVM *vm);
JNIEnv *host_env();          // host env for the calling thread (attaches if needed)
u32 guest_vm();              // guest JavaVM*
u32 guest_env();             // guest JNIEnv* for the calling thread

// Reference translation. Guest handles: 0 = null, low 2 bits = kind (1 local, 2 global, 3 weak).
u32 to_guest(jobject o);     // registers a local reference for the current frame
jobject to_host(u32 h);

// Frames around Java -> guest native calls.
struct NativeFrame {
    NativeFrame();
    ~NativeFrame();
    size_t mark;
};
}  // namespace jni
