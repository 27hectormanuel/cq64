// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Turns ordinary C++ functions into guest thunks by decoding AAPCS (softfp) arguments.
//
//   static u32 my_strlen(G<const char> s) { ... }
//   THUNK(strlen, my_strlen);
//
// Supported parameter types: integers up to 64 bits, float, double, G<T> guest pointers.
#include "cpu.h"
#include "loader.h"

#include <tuple>
#include <type_traits>

namespace abi {
struct Cursor {
    GuestThread &t;
    int ncrn = 0;
    u32 nsaa = 0;
    u32 word() {
        if (ncrn < 4) return t.r(ncrn++);
        u32 v = rd<u32>(t.sp() + nsaa);
        nsaa += 4;
        return v;
    }
    u64 dword() {
        if (ncrn & 1) ncrn++;
        if (ncrn <= 2) {
            u64 v = t.r(ncrn) | ((u64)t.r(ncrn + 1) << 32);
            ncrn += 2;
            return v;
        }
        ncrn = 4;
        nsaa = (nsaa + 7) & ~7u;
        u64 v = rd<u64>(t.sp() + nsaa);
        nsaa += 8;
        return v;
    }
};

template <class T, class = void> struct Arg;
template <class T> struct Arg<T, std::enable_if_t<std::is_integral_v<T> && sizeof(T) <= 4>> {
    static T get(Cursor &c) { return (T)c.word(); }
};
template <class T> struct Arg<T, std::enable_if_t<std::is_integral_v<T> && sizeof(T) == 8>> {
    static T get(Cursor &c) { return (T)c.dword(); }
};
template <class T> struct Arg<T, std::enable_if_t<std::is_enum_v<T>>> {
    static T get(Cursor &c) { return (T)c.word(); }
};
template <> struct Arg<float> {
    static float get(Cursor &c) { u32 w = c.word(); float f; memcpy(&f, &w, 4); return f; }
};
template <> struct Arg<double> {
    static double get(Cursor &c) { u64 w = c.dword(); double d; memcpy(&d, &w, 8); return d; }
};
template <class T> struct Arg<G<T>> {
    static G<T> get(Cursor &c) { return G<T>{c.word()}; }
};

template <class T, class = void> struct Ret;
template <class T> struct Ret<T, std::enable_if_t<(std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= 4>> {
    static void set(GuestThread &t, T v) {
        // Sign/zero extension matches what ARM callers expect from small return types.
        t.r(0) = (u32)(std::conditional_t<std::is_signed_v<T>, s32, u32>)v;
    }
};
template <class T> struct Ret<T, std::enable_if_t<std::is_integral_v<T> && sizeof(T) == 8>> {
    static void set(GuestThread &t, T v) { t.ret64((u64)v); }
};
template <> struct Ret<float> {
    static void set(GuestThread &t, float f) { u32 w; memcpy(&w, &f, 4); t.r(0) = w; }
};
template <> struct Ret<double> {
    static void set(GuestThread &t, double d) { u64 w; memcpy(&w, &d, 8); t.ret64(w); }
};
template <class T> struct Ret<G<T>> {
    static void set(GuestThread &t, G<T> p) { t.r(0) = p.a; }
};

template <class R, class... A> struct Invoker {
    template <R (*F)(A...)> static void run(GuestThread &t) {
        Cursor c{t};
        std::tuple<A...> args{Arg<A>::get(c)...};  // braced init: evaluated left to right
        if constexpr (std::is_void_v<R>) std::apply(F, args);
        else Ret<R>::set(t, std::apply(F, args));
    }
};
template <class R, class... A> Invoker<R, A...> invoker_of(R (*)(A...));
template <auto F> void wrapped(GuestThread &t) { decltype(invoker_of(F))::template run<F>(t); }

struct Registrar {
    Registrar(const char *name, ThunkFn fn) { thunks::register_thunk(name, fn); }
};
}  // namespace abi

#define THUNK_CAT2(a, b) a##b
#define THUNK_CAT(a, b) THUNK_CAT2(a, b)
// Register a typed C++ function as the implementation of guest import `name`.
#define THUNK(name, fn) static abi::Registrar THUNK_CAT(reg_, __COUNTER__)(#name, &abi::wrapped<fn>)
// Register a raw handler taking GuestThread& (for varargs or unusual conventions).
#define THUNK_RAW(name, fn) static abi::Registrar THUNK_CAT(reg_, __COUNTER__)(#name, fn)
