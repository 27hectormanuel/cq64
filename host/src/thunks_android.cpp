// SPDX-License-Identifier: GPL-3.0-or-later
// libandroid imports. ANativeWindow pointers are 64-bit, so the guest gets small handles.
#include "abi.h"
#include "devtools.h"
#include "jni_bridge.h"

#include <android/native_window_jni.h>
#include <EGL/egl.h>

#include <mutex>
#include <time.h>
#include <vector>

namespace {
std::mutex g_lock;
std::vector<ANativeWindow *> g_windows{nullptr};

u32 window_handle(ANativeWindow *w) {
    if (!w) return 0;
    std::lock_guard<std::mutex> lk(g_lock);
    for (size_t i = 1; i < g_windows.size(); i++)
        if (g_windows[i] == w) return (u32)i;
    g_windows.push_back(w);
    return (u32)g_windows.size() - 1;
}
ANativeWindow *window_of(u32 h) {
    std::lock_guard<std::mutex> lk(g_lock);
    return h < g_windows.size() ? g_windows[h] : nullptr;
}

u32 t_ANativeWindow_fromSurface(u32, u32 surface) {
    return window_handle(ANativeWindow_fromSurface(jni::host_env(), jni::to_host(surface)));
}
THUNK(ANativeWindow_fromSurface, t_ANativeWindow_fromSurface);
void t_ANativeWindow_release(u32 h) {
    ANativeWindow *w = window_of(h);
    if (!w) return;
    ANativeWindow_release(w);
    std::lock_guard<std::mutex> lk(g_lock);
    g_windows[h] = nullptr;
}
THUNK(ANativeWindow_release, t_ANativeWindow_release);
int t_ANativeWindow_setBuffersGeometry(u32 h, int w, int ht, int fmt) {
    ANativeWindow *win = window_of(h);
    return win ? ANativeWindow_setBuffersGeometry(win, w, ht, fmt) : -1;
}
THUNK(ANativeWindow_setBuffersGeometry, t_ANativeWindow_setBuffersGeometry);
// ---------------------------------------------------------------- EGL
// EGL objects are host pointers; the guest sees indices into per-type tables.
template <class T> struct Handles {
    std::mutex lock;
    std::vector<T> v{nullptr};
    u32 to_guest(T p) {
        if (!p) return 0;
        std::lock_guard<std::mutex> lk(lock);
        for (size_t i = 1; i < v.size(); i++)
            if (v[i] == p) return (u32)i;
        v.push_back(p);
        return (u32)v.size() - 1;
    }
    T to_host(u32 h) {
        std::lock_guard<std::mutex> lk(lock);
        return h < v.size() ? v[h] : nullptr;
    }
};
Handles<EGLDisplay> g_dpys;
Handles<EGLConfig> g_cfgs;
Handles<EGLContext> g_ctxs;
Handles<EGLSurface> g_surfs;

u32 t_eglGetDisplay(u32) { return g_dpys.to_guest(eglGetDisplay(EGL_DEFAULT_DISPLAY)); }
THUNK(eglGetDisplay, t_eglGetDisplay);
u32 t_eglInitialize(u32 d, G<EGLint> maj, G<EGLint> min) { return eglInitialize(g_dpys.to_host(d), maj, min); }
THUNK(eglInitialize, t_eglInitialize);
u32 t_eglTerminate(u32 d) { return eglTerminate(g_dpys.to_host(d)); }
THUNK(eglTerminate, t_eglTerminate);
s32 t_eglGetError() { return eglGetError(); }
THUNK(eglGetError, t_eglGetError);
u32 t_eglGetProcAddress(G<const char> name) {
    u32 a = loader::lookup(name);
    if (!a) LOGW("eglGetProcAddress(%s): no implementation", name.get());
    return a;
}
THUNK(eglGetProcAddress, t_eglGetProcAddress);
u32 t_eglChooseConfig(u32 d, G<const EGLint> attribs, u32 configs, s32 size, G<EGLint> num) {
    std::vector<EGLConfig> hc(size > 0 ? size : 0);
    EGLBoolean r = eglChooseConfig(g_dpys.to_host(d), attribs, configs ? hc.data() : nullptr, size, num);
    if (r && configs)
        for (EGLint i = 0; i < *num.get(); i++) wr<u32>(configs + i * 4, g_cfgs.to_guest(hc[i]));
    return r;
}
THUNK(eglChooseConfig, t_eglChooseConfig);
u32 t_eglGetConfigs(u32 d, u32 configs, s32 size, G<EGLint> num) {
    std::vector<EGLConfig> hc(size > 0 ? size : 0);
    EGLBoolean r = eglGetConfigs(g_dpys.to_host(d), configs ? hc.data() : nullptr, size, num);
    if (r && configs)
        for (EGLint i = 0; i < *num.get(); i++) wr<u32>(configs + i * 4, g_cfgs.to_guest(hc[i]));
    return r;
}
THUNK(eglGetConfigs, t_eglGetConfigs);
u32 t_eglGetConfigAttrib(u32 d, u32 c, s32 attr, G<EGLint> v) {
    return eglGetConfigAttrib(g_dpys.to_host(d), g_cfgs.to_host(c), attr, v);
}
THUNK(eglGetConfigAttrib, t_eglGetConfigAttrib);
u32 t_eglCreateContext(u32 d, u32 c, u32 share, G<const EGLint> attribs) {
    EGLContext ctx = eglCreateContext(g_dpys.to_host(d), g_cfgs.to_host(c), g_ctxs.to_host(share), attribs);
    if (!ctx) LOGE("eglCreateContext failed: 0x%x", eglGetError());
    return g_ctxs.to_guest(ctx);
}
THUNK(eglCreateContext, t_eglCreateContext);
u32 t_eglDestroyContext(u32 d, u32 c) { return eglDestroyContext(g_dpys.to_host(d), g_ctxs.to_host(c)); }
THUNK(eglDestroyContext, t_eglDestroyContext);
u32 t_eglCreateWindowSurface(u32 d, u32 c, u32 win, G<const EGLint> attribs) {
    EGLSurface s = eglCreateWindowSurface(g_dpys.to_host(d), g_cfgs.to_host(c), window_of(win), attribs);
    if (!s) LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
    return g_surfs.to_guest(s);
}
THUNK(eglCreateWindowSurface, t_eglCreateWindowSurface);
u32 t_eglDestroySurface(u32 d, u32 s) { return eglDestroySurface(g_dpys.to_host(d), g_surfs.to_host(s)); }
THUNK(eglDestroySurface, t_eglDestroySurface);
u32 t_eglMakeCurrent(u32 d, u32 dr, u32 rd, u32 c) {
    return eglMakeCurrent(g_dpys.to_host(d), g_surfs.to_host(dr), g_surfs.to_host(rd), g_ctxs.to_host(c));
}
THUNK(eglMakeCurrent, t_eglMakeCurrent);
u32 t_eglSwapBuffers(u32 d, u32 s) {
    static int frames = 0;
    static struct timespec last{};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    frames++;
    double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
    if (dt >= 5.0) {
        if (last.tv_sec) LOGI("fps: %.1f", frames / dt);
        frames = 0;
        last = now;
    }
    devtools::frame_presented();
    return eglSwapBuffers(g_dpys.to_host(d), g_surfs.to_host(s));
}
THUNK(eglSwapBuffers, t_eglSwapBuffers);
u32 t_eglSwapInterval(u32 d, s32 i) { return eglSwapInterval(g_dpys.to_host(d), i); }
THUNK(eglSwapInterval, t_eglSwapInterval);
u32 t_eglQuerySurface(u32 d, u32 s, s32 attr, G<EGLint> v) {
    return eglQuerySurface(g_dpys.to_host(d), g_surfs.to_host(s), attr, v);
}
THUNK(eglQuerySurface, t_eglQuerySurface);
u32 t_eglWaitGL() { return eglWaitGL(); }
THUNK(eglWaitGL, t_eglWaitGL);
u32 t_eglWaitNative(s32 e) { return eglWaitNative(e); }
THUNK(eglWaitNative, t_eglWaitNative);
u32 t_eglGetCurrentContext() { return g_ctxs.to_guest(eglGetCurrentContext()); }
THUNK(eglGetCurrentContext, t_eglGetCurrentContext);
u32 t_eglGetCurrentDisplay() { return g_dpys.to_guest(eglGetCurrentDisplay()); }
THUNK(eglGetCurrentDisplay, t_eglGetCurrentDisplay);
u32 t_eglGetCurrentSurface(s32 w) { return g_surfs.to_guest(eglGetCurrentSurface(w)); }
THUNK(eglGetCurrentSurface, t_eglGetCurrentSurface);
u32 t_eglQueryString(u32 d, s32 name) {
    const char *s = eglQueryString(g_dpys.to_host(d), name);
    return s ? mem::hstrdup(s) : 0;  // rare; small leak acceptable
}
THUNK(eglQueryString, t_eglQueryString);
u32 t_eglBindAPI(u32 api) { return eglBindAPI(api); }
THUNK(eglBindAPI, t_eglBindAPI);
}  // namespace
