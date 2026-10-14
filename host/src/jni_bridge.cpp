// SPDX-License-Identifier: CC0-1.0
// Guest-facing JNI: a 32-bit JNIEnv/JavaVM whose function tables trap into the host,
// where each call is translated onto the real (64-bit) JNIEnv.
#include "jni_bridge.h"
#include "abi.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace jni {
namespace {
JavaVM *g_vm;
u32 g_gvm, g_vm_table, g_env_table;

struct ThreadState {
    std::vector<jobject> locals;
    std::vector<size_t> frames;  // marks for guest PushLocalFrame
    JNIEnv *env = nullptr;
    u32 genv = 0;
    bool attached_by_us = false;
};
thread_local ThreadState t_state;

std::mutex g_ref_lock;
std::vector<jobject> g_globals, g_weaks;
std::vector<u32> g_globals_free, g_weaks_free;

struct MethodInfo {
    jmethodID id;
    std::string sig;
};
std::mutex g_id_lock;
std::vector<MethodInfo> g_methods;
std::unordered_map<jmethodID, u32> g_method_ids;
std::vector<jfieldID> g_fields;
std::unordered_map<jfieldID, u32> g_field_ids;

struct Mirror {
    void *host;
    u32 guest, size;
    jobject buffer;  // global reference: keeps the buffer, and so `host`, from being collected
};
std::mutex g_mirror_lock;
std::unordered_map<void *, Mirror> g_mirrors;

JNIEnv *E() { return host_env(); }

u32 method_to_guest(jmethodID id, const char *sig) {
    if (!id) return 0;
    std::lock_guard<std::mutex> lk(g_id_lock);
    auto it = g_method_ids.find(id);
    if (it != g_method_ids.end()) return it->second;
    g_methods.push_back({id, sig});
    u32 g = (u32)g_methods.size();
    g_method_ids[id] = g;
    return g;
}
MethodInfo method_info(u32 g) {
    std::lock_guard<std::mutex> lk(g_id_lock);
    if (!g || g > g_methods.size()) fatal("invalid guest jmethodID %u", g);
    return g_methods[g - 1];
}
u32 field_to_guest(jfieldID id) {
    if (!id) return 0;
    std::lock_guard<std::mutex> lk(g_id_lock);
    auto it = g_field_ids.find(id);
    if (it != g_field_ids.end()) return it->second;
    g_fields.push_back(id);
    u32 g = (u32)g_fields.size();
    g_field_ids[id] = g;
    return g;
}
jfieldID field_of(u32 g) {
    std::lock_guard<std::mutex> lk(g_id_lock);
    if (!g || g > g_fields.size()) fatal("invalid guest jfieldID %u", g);
    return g_fields[g - 1];
}

u32 add_ref(std::vector<jobject> &tab, std::vector<u32> &free_list, jobject o, u32 kind) {
    std::lock_guard<std::mutex> lk(g_ref_lock);
    u32 i;
    if (!free_list.empty()) {
        i = free_list.back();
        free_list.pop_back();
        tab[i] = o;
    } else {
        i = (u32)tab.size();
        tab.push_back(o);
    }
    return ((i + 1) << 2) | kind;
}

struct Args {
    abi::Cursor c;
    explicit Args(GuestThread &t) : c{t} {}
    u32 w() { return c.word(); }
    u64 d() { return c.dword(); }
    float f() { u32 x = w(); float r; memcpy(&r, &x, 4); return r; }
    double dd() { u64 x = d(); double r; memcpy(&r, &x, 8); return r; }
    jobject o() { return to_host(w()); }
    const char *s() { return (const char *)g2h(w()); }
};

void ret_float(GuestThread &t, float f) { u32 w; memcpy(&w, &f, 4); t.ret32(w); }
void ret_double(GuestThread &t, double d) { u64 w; memcpy(&w, &d, 8); t.ret64(w); }

char type_kind(char c) { return (c == 'L' || c == '[') ? 'L' : c; }
const char *skip_type(const char *p) {
    while (*p == '[') p++;
    if (*p == 'L') {
        while (*p && *p != ';') p++;
    }
    return *p ? p + 1 : p;
}
char return_kind(const std::string &sig) {
    auto pos = sig.find(')');
    return pos == std::string::npos ? 'V' : type_kind(sig[pos + 1]);
}

// mode 0: variadic (guest regs/stack), 1: va_list pointer, 2: jvalue array
void parse_args(const std::string &sig, int mode, Args &a, std::vector<jvalue> &out) {
    u32 va = mode == 1 ? a.w() : 0;
    u32 arr = mode == 2 ? a.w() : 0;
    const char *p = sig.c_str();
    if (*p == '(') p++;
    for (int i = 0; *p && *p != ')'; i++) {
        char k = type_kind(*p);
        p = skip_type(p);
        jvalue v{};
        if (mode == 0) {
            switch (k) {
            case 'J': v.j = (jlong)a.d(); break;
            case 'D': v.d = a.dd(); break;
            case 'F': v.f = (float)a.dd(); break;  // promoted to double
            case 'L': v.l = a.o(); break;
            case 'Z': v.z = (jboolean)a.w(); break;
            case 'B': v.b = (jbyte)a.w(); break;
            case 'C': v.c = (jchar)a.w(); break;
            case 'S': v.s = (jshort)a.w(); break;
            default: v.i = (jint)a.w(); break;
            }
        } else if (mode == 1) {
            if (k == 'J' || k == 'D' || k == 'F') {
                va = (va + 7) & ~7u;
                u64 x = rd<u64>(va);
                va += 8;
                if (k == 'J') v.j = (jlong)x;
                else {
                    double d;
                    memcpy(&d, &x, 8);
                    if (k == 'D') v.d = d;
                    else v.f = (float)d;
                }
            } else {
                u32 x = rd<u32>(va);
                va += 4;
                switch (k) {
                case 'L': v.l = to_host(x); break;
                case 'Z': v.z = (jboolean)x; break;
                case 'B': v.b = (jbyte)x; break;
                case 'C': v.c = (jchar)x; break;
                case 'S': v.s = (jshort)x; break;
                default: v.i = (jint)x; break;
                }
            }
        } else {
            u32 slot = arr + i * 8;
            switch (k) {
            case 'J': v.j = rd<s64>(slot); break;
            case 'D': v.d = rd<double>(slot); break;
            case 'F': v.f = rd<float>(slot); break;
            case 'L': v.l = to_host(rd<u32>(slot)); break;
            case 'Z': v.z = rd<u8>(slot); break;
            case 'B': v.b = rd<s8>(slot); break;
            case 'C': v.c = rd<u16>(slot); break;
            case 'S': v.s = rd<s16>(slot); break;
            default: v.i = rd<s32>(slot); break;
            }
        }
        out.push_back(v);
    }
}

void set_ret(GuestThread &t, char k, const jvalue &r) {
    switch (k) {
    case 'L': t.ret32(to_guest(r.l)); break;
    case 'Z': t.ret32(r.z); break;
    case 'B': t.ret32((u32)(s32)r.b); break;
    case 'C': t.ret32(r.c); break;
    case 'S': t.ret32((u32)(s32)r.s); break;
    case 'I': t.ret32((u32)r.i); break;
    case 'J': t.ret64((u64)r.j); break;
    case 'F': ret_float(t, r.f); break;
    case 'D': ret_double(t, r.d); break;
    default: break;
    }
}

// KIND: 0 virtual, 1 nonvirtual, 2 static
template <char K, int MODE, int KIND> void jni_call(GuestThread &t) {
    Args a(t);
    a.w();  // env
    jobject obj = a.o();
    jclass cls = KIND == 1 ? (jclass)a.o() : nullptr;
    u32 gm = a.w();
    MethodInfo mi = method_info(gm);
    std::vector<jvalue> v;
    parse_args(mi.sig, MODE, a, v);
    JNIEnv *e = E();
    jvalue r{};
    const jvalue *av = v.data();
#define JCALL(Name, field)                                                                   \
    if (KIND == 0) field e->Call##Name##MethodA(obj, mi.id, av);                             \
    else if (KIND == 1) field e->CallNonvirtual##Name##MethodA(obj, cls, mi.id, av);         \
    else field e->CallStatic##Name##MethodA((jclass)obj, mi.id, av);
    if constexpr (K == 'L') { JCALL(Object, r.l =) }
    else if constexpr (K == 'Z') { JCALL(Boolean, r.z =) }
    else if constexpr (K == 'B') { JCALL(Byte, r.b =) }
    else if constexpr (K == 'C') { JCALL(Char, r.c =) }
    else if constexpr (K == 'S') { JCALL(Short, r.s =) }
    else if constexpr (K == 'I') { JCALL(Int, r.i =) }
    else if constexpr (K == 'J') { JCALL(Long, r.j =) }
    else if constexpr (K == 'F') { JCALL(Float, r.f =) }
    else if constexpr (K == 'D') { JCALL(Double, r.d =) }
    else { JCALL(Void, ) }
#undef JCALL
    if (e->ExceptionCheck()) LOGW("Java exception pending after guest call of method sig %s", mi.sig.c_str());
    set_ret(t, K, r);
}

template <int MODE> void jni_new_object(GuestThread &t) {
    Args a(t);
    a.w();
    jclass cls = (jclass)a.o();
    MethodInfo mi = method_info(a.w());
    std::vector<jvalue> v;
    parse_args(mi.sig, MODE, a, v);
    t.ret32(to_guest(E()->NewObjectA(cls, mi.id, v.data())));
}

// Field access. STATIC selects Get/SetStatic*Field.
template <char K, bool STATIC> void jni_get_field(GuestThread &t) {
    Args a(t);
    a.w();
    jobject obj = a.o();
    jfieldID f = field_of(a.w());
    JNIEnv *e = E();
    jvalue r{};
#define JGET(Name, field)                                   \
    if (STATIC) field e->GetStatic##Name##Field((jclass)obj, f); \
    else field e->Get##Name##Field(obj, f);
    if constexpr (K == 'L') { JGET(Object, r.l =) }
    else if constexpr (K == 'Z') { JGET(Boolean, r.z =) }
    else if constexpr (K == 'B') { JGET(Byte, r.b =) }
    else if constexpr (K == 'C') { JGET(Char, r.c =) }
    else if constexpr (K == 'S') { JGET(Short, r.s =) }
    else if constexpr (K == 'I') { JGET(Int, r.i =) }
    else if constexpr (K == 'J') { JGET(Long, r.j =) }
    else if constexpr (K == 'F') { JGET(Float, r.f =) }
    else if constexpr (K == 'D') { JGET(Double, r.d =) }
#undef JGET
    set_ret(t, K, r);
}

template <char K, bool STATIC> void jni_set_field(GuestThread &t) {
    Args a(t);
    a.w();
    jobject obj = a.o();
    jfieldID f = field_of(a.w());
    JNIEnv *e = E();
#define JSET(Name, val)                                          \
    if (STATIC) e->SetStatic##Name##Field((jclass)obj, f, val); \
    else e->Set##Name##Field(obj, f, val);
    if constexpr (K == 'L') { JSET(Object, a.o()) }
    else if constexpr (K == 'Z') { JSET(Boolean, (jboolean)a.w()) }
    else if constexpr (K == 'B') { JSET(Byte, (jbyte)a.w()) }
    else if constexpr (K == 'C') { JSET(Char, (jchar)a.w()) }
    else if constexpr (K == 'S') { JSET(Short, (jshort)a.w()) }
    else if constexpr (K == 'I') { JSET(Int, (jint)a.w()) }
    else if constexpr (K == 'J') { JSET(Long, (jlong)a.d()) }
    else if constexpr (K == 'F') { JSET(Float, a.f()) }
    else if constexpr (K == 'D') { JSET(Double, a.dd()) }
#undef JSET
}

// Primitive arrays
template <char K> struct ArrT;
#define ARRT(K_, Name_, T_)                                                                           \
    template <> struct ArrT<K_> {                                                                     \
        using T = T_;                                                                                 \
        static jarray make(JNIEnv *e, jsize n) { return e->New##Name_##Array(n); }                    \
        static void get(JNIEnv *e, jarray a, jsize s, jsize n, T *b) { e->Get##Name_##ArrayRegion((T_##Array)a, s, n, b); } \
        static void set(JNIEnv *e, jarray a, jsize s, jsize n, const T *b) { e->Set##Name_##ArrayRegion((T_##Array)a, s, n, b); } \
    };
ARRT('Z', Boolean, jboolean)
ARRT('B', Byte, jbyte)
ARRT('C', Char, jchar)
ARRT('S', Short, jshort)
ARRT('I', Int, jint)
ARRT('J', Long, jlong)
ARRT('F', Float, jfloat)
ARRT('D', Double, jdouble)
#undef ARRT

template <char K> void jni_new_array(GuestThread &t) {
    Args a(t);
    a.w();
    t.ret32(to_guest(ArrT<K>::make(E(), (jsize)a.w())));
}
template <char K> void jni_get_elements(GuestThread &t) {
    Args a(t);
    a.w();
    jarray arr = (jarray)a.o();
    u32 is_copy = a.w();
    JNIEnv *e = E();
    jsize n = e->GetArrayLength(arr);
    u32 g = mem::halloc((u32)(n * sizeof(typename ArrT<K>::T)) + 8);
    ArrT<K>::get(e, arr, 0, n, (typename ArrT<K>::T *)g2h(g));
    if (is_copy) wr<u8>(is_copy, 1);
    t.ret32(g);
}
template <char K> void jni_release_elements(GuestThread &t) {
    Args a(t);
    a.w();
    jarray arr = (jarray)a.o();
    u32 elems = a.w();
    s32 mode = (s32)a.w();
    JNIEnv *e = E();
    if (mode != JNI_ABORT) ArrT<K>::set(e, arr, 0, e->GetArrayLength(arr), (typename ArrT<K>::T *)g2h(elems));
    if (mode != JNI_COMMIT) mem::hfree(elems);
}
template <char K> void jni_get_region(GuestThread &t) {
    Args a(t);
    a.w();
    jarray arr = (jarray)a.o();
    jsize s = (jsize)a.w(), n = (jsize)a.w();
    ArrT<K>::get(E(), arr, s, n, (typename ArrT<K>::T *)g2h(a.w()));
}
template <char K> void jni_set_region(GuestThread &t) {
    Args a(t);
    a.w();
    jarray arr = (jarray)a.o();
    jsize s = (jsize)a.w(), n = (jsize)a.w();
    ArrT<K>::set(E(), arr, s, n, (const typename ArrT<K>::T *)g2h(a.w()));
}

char array_kind(JNIEnv *e, jarray arr) {
    jclass c = e->GetObjectClass(arr);
    jclass cc = e->FindClass("java/lang/Class");
    jmethodID getName = e->GetMethodID(cc, "getName", "()Ljava/lang/String;");
    auto name = (jstring)e->CallObjectMethod(c, getName);
    const char *s = e->GetStringUTFChars(name, nullptr);
    char k = s[1];
    e->ReleaseStringUTFChars(name, s);
    e->DeleteLocalRef(name);
    e->DeleteLocalRef(cc);
    e->DeleteLocalRef(c);
    return k;
}
int elem_size(char k) { return k == 'J' || k == 'D' ? 8 : k == 'I' || k == 'F' ? 4 : k == 'C' || k == 'S' ? 2 : 1; }

// ------------------------------------------------------------------ simple entries
void j_GetVersion(GuestThread &t) { t.ret32(JNI_VERSION_1_6); }
void j_FindClass(GuestThread &t) {
    Args a(t);
    a.w();
    const char *name = a.s();
    jclass c = E()->FindClass(name);
    if (!c) {
        LOGW("guest FindClass(%s) failed", name);
        E()->ExceptionClear();
    }
    t.ret32(to_guest(c));
}
void j_GetSuperclass(GuestThread &t) { Args a(t); a.w(); t.ret32(to_guest(E()->GetSuperclass((jclass)a.o()))); }
void j_IsAssignableFrom(GuestThread &t) {
    Args a(t); a.w();
    jclass x = (jclass)a.o(), y = (jclass)a.o();
    t.ret32(E()->IsAssignableFrom(x, y));
}
void j_Throw(GuestThread &t) { Args a(t); a.w(); t.ret32(E()->Throw((jthrowable)a.o())); }
void j_ThrowNew(GuestThread &t) {
    Args a(t); a.w();
    jclass c = (jclass)a.o();
    t.ret32(E()->ThrowNew(c, a.s()));
}
void j_ExceptionOccurred(GuestThread &t) { t.ret32(to_guest(E()->ExceptionOccurred())); }
void j_ExceptionDescribe(GuestThread &) { E()->ExceptionDescribe(); }
void j_ExceptionClear(GuestThread &) { E()->ExceptionClear(); }
void j_ExceptionCheck(GuestThread &t) { t.ret32(E()->ExceptionCheck()); }
void j_FatalError(GuestThread &t) { Args a(t); a.w(); fatal("guest JNI FatalError: %s", a.s()); }
void j_PushLocalFrame(GuestThread &t) {
    Args a(t); a.w();
    jint r = E()->PushLocalFrame((jint)a.w());
    if (r == 0) t_state.frames.push_back(t_state.locals.size());
    t.ret32((u32)r);
}
void j_PopLocalFrame(GuestThread &t) {
    Args a(t); a.w();
    jobject res = a.o();
    jobject nr = E()->PopLocalFrame(res);
    if (!t_state.frames.empty()) {
        t_state.locals.resize(t_state.frames.back());
        t_state.frames.pop_back();
    }
    t.ret32(to_guest(nr));
}
void j_NewGlobalRef(GuestThread &t) {
    Args a(t); a.w();
    jobject o = a.o();
    if (!o) return t.ret32(0);
    t.ret32(add_ref(g_globals, g_globals_free, E()->NewGlobalRef(o), 2));
}
void j_DeleteGlobalRef(GuestThread &t) {
    Args a(t); a.w();
    u32 h = a.w();
    if ((h & 3) != 2) return;
    u32 i = (h >> 2) - 1;
    jobject o;
    {
        std::lock_guard<std::mutex> lk(g_ref_lock);
        if (i >= g_globals.size()) return;
        o = g_globals[i];
        g_globals[i] = nullptr;
        g_globals_free.push_back(i);
    }
    if (o) E()->DeleteGlobalRef(o);
}
void j_DeleteLocalRef(GuestThread &t) {
    Args a(t); a.w();
    u32 h = a.w();
    if ((h & 3) != 1) return;
    u32 i = (h >> 2) - 1;
    if (i < t_state.locals.size() && t_state.locals[i]) {
        E()->DeleteLocalRef(t_state.locals[i]);
        t_state.locals[i] = nullptr;
    }
}
void j_IsSameObject(GuestThread &t) {
    Args a(t); a.w();
    jobject x = a.o(), y = a.o();
    t.ret32(E()->IsSameObject(x, y));
}
void j_NewLocalRef(GuestThread &t) { Args a(t); a.w(); t.ret32(to_guest(E()->NewLocalRef(a.o()))); }
void j_EnsureLocalCapacity(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->EnsureLocalCapacity((jint)a.w())); }
void j_AllocObject(GuestThread &t) { Args a(t); a.w(); t.ret32(to_guest(E()->AllocObject((jclass)a.o()))); }
void j_GetObjectClass(GuestThread &t) { Args a(t); a.w(); t.ret32(to_guest(E()->GetObjectClass(a.o()))); }
void j_IsInstanceOf(GuestThread &t) {
    Args a(t); a.w();
    jobject o = a.o();
    t.ret32(E()->IsInstanceOf(o, (jclass)a.o()));
}
template <bool STATIC> void j_GetMethodID(GuestThread &t) {
    Args a(t); a.w();
    jclass c = (jclass)a.o();
    const char *name = a.s(), *sig = a.s();
    JNIEnv *e = E();
    jmethodID m = STATIC ? e->GetStaticMethodID(c, name, sig) : e->GetMethodID(c, name, sig);
    if (!m) {
        LOGW("guest Get%sMethodID(%s %s) failed", STATIC ? "Static" : "", name, sig);
        e->ExceptionClear();
    }
    t.ret32(method_to_guest(m, sig));
}
template <bool STATIC> void j_GetFieldID(GuestThread &t) {
    Args a(t); a.w();
    jclass c = (jclass)a.o();
    const char *name = a.s(), *sig = a.s();
    JNIEnv *e = E();
    jfieldID f = STATIC ? e->GetStaticFieldID(c, name, sig) : e->GetFieldID(c, name, sig);
    if (!f) {
        LOGW("guest Get%sFieldID(%s %s) failed", STATIC ? "Static" : "", name, sig);
        e->ExceptionClear();
    }
    t.ret32(field_to_guest(f));
}

void j_NewString(GuestThread &t) {
    Args a(t); a.w();
    auto *chars = (const jchar *)g2h(a.w());
    t.ret32(to_guest(E()->NewString(chars, (jsize)a.w())));
}
void j_GetStringLength(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->GetStringLength((jstring)a.o())); }
void j_GetStringChars(GuestThread &t) {
    Args a(t); a.w();
    auto s = (jstring)a.o();
    u32 is_copy = a.w();
    JNIEnv *e = E();
    jsize n = e->GetStringLength(s);
    u32 g = mem::halloc((u32)(n + 1) * 2);
    e->GetStringRegion(s, 0, n, (jchar *)g2h(g));
    if (is_copy) wr<u8>(is_copy, 1);
    t.ret32(g);
}
void j_ReleaseStringChars(GuestThread &t) { Args a(t); a.w(); a.w(); mem::hfree(a.w()); }
void j_NewStringUTF(GuestThread &t) {
    Args a(t); a.w();
    u32 p = a.w();
    t.ret32(p ? to_guest(E()->NewStringUTF((const char *)g2h(p))) : 0);
}
void j_GetStringUTFLength(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->GetStringUTFLength((jstring)a.o())); }
void j_GetStringUTFChars(GuestThread &t) {
    Args a(t); a.w();
    auto s = (jstring)a.o();
    u32 is_copy = a.w();
    JNIEnv *e = E();
    const char *c = e->GetStringUTFChars(s, nullptr);
    u32 g = mem::hstrdup(c ? c : "");
    if (c) e->ReleaseStringUTFChars(s, c);
    if (is_copy) wr<u8>(is_copy, 1);
    t.ret32(g);
}
void j_ReleaseStringUTFChars(GuestThread &t) { Args a(t); a.w(); a.w(); mem::hfree(a.w()); }
void j_GetStringRegion(GuestThread &t) {
    Args a(t); a.w();
    auto s = (jstring)a.o();
    jsize st = (jsize)a.w(), n = (jsize)a.w();
    E()->GetStringRegion(s, st, n, (jchar *)g2h(a.w()));
}
void j_GetStringUTFRegion(GuestThread &t) {
    Args a(t); a.w();
    auto s = (jstring)a.o();
    jsize st = (jsize)a.w(), n = (jsize)a.w();
    E()->GetStringUTFRegion(s, st, n, (char *)g2h(a.w()));
}

void j_GetArrayLength(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->GetArrayLength((jarray)a.o())); }
void j_NewObjectArray(GuestThread &t) {
    Args a(t); a.w();
    jsize n = (jsize)a.w();
    jclass c = (jclass)a.o();
    jobject init = a.o();
    t.ret32(to_guest(E()->NewObjectArray(n, c, init)));
}
void j_GetObjectArrayElement(GuestThread &t) {
    Args a(t); a.w();
    auto arr = (jobjectArray)a.o();
    t.ret32(to_guest(E()->GetObjectArrayElement(arr, (jsize)a.w())));
}
void j_SetObjectArrayElement(GuestThread &t) {
    Args a(t); a.w();
    auto arr = (jobjectArray)a.o();
    jsize i = (jsize)a.w();
    E()->SetObjectArrayElement(arr, i, a.o());
}
void j_GetPrimitiveArrayCritical(GuestThread &t) {
    Args a(t); a.w();
    auto arr = (jarray)a.o();
    u32 is_copy = a.w();
    JNIEnv *e = E();
    char k = array_kind(e, arr);
    jsize n = e->GetArrayLength(arr);
    int es = elem_size(k);
    u32 g = mem::halloc((u32)(n * es) + 8);
    void *src = e->GetPrimitiveArrayCritical(arr, nullptr);
    memcpy(g2h(g), src, (size_t)n * es);
    e->ReleasePrimitiveArrayCritical(arr, src, JNI_ABORT);
    if (is_copy) wr<u8>(is_copy, 1);
    t.ret32(g);
}
void j_ReleasePrimitiveArrayCritical(GuestThread &t) {
    Args a(t); a.w();
    auto arr = (jarray)a.o();
    u32 elems = a.w();
    s32 mode = (s32)a.w();
    JNIEnv *e = E();
    if (mode != JNI_ABORT) {
        int es = elem_size(array_kind(e, arr));
        jsize n = e->GetArrayLength(arr);
        void *dst = e->GetPrimitiveArrayCritical(arr, nullptr);
        memcpy(dst, g2h(elems), (size_t)n * es);
        e->ReleasePrimitiveArrayCritical(arr, dst, 0);
    }
    if (mode != JNI_COMMIT) mem::hfree(elems);
}
void j_RegisterNatives(GuestThread &t) {
    Args a(t); a.w();
    a.w();
    u32 methods = a.w();
    s32 n = (s32)a.w();
    for (s32 i = 0; i < n; i++)
        LOGE("guest RegisterNatives(%s %s) is not supported", (const char *)g2h(rd<u32>(methods + i * 12)),
             (const char *)g2h(rd<u32>(methods + i * 12 + 4)));
    t.ret32(0);
}
void j_UnregisterNatives(GuestThread &t) { t.ret32(0); }
void j_MonitorEnter(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->MonitorEnter(a.o())); }
void j_MonitorExit(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->MonitorExit(a.o())); }
void j_GetJavaVM(GuestThread &t) {
    Args a(t); a.w();
    wr<u32>(a.w(), g_gvm);
    t.ret32(0);
}
void j_NewWeakGlobalRef(GuestThread &t) {
    Args a(t); a.w();
    jobject o = a.o();
    if (!o) return t.ret32(0);
    t.ret32(add_ref(g_weaks, g_weaks_free, E()->NewWeakGlobalRef(o), 3));
}
void j_DeleteWeakGlobalRef(GuestThread &t) {
    Args a(t); a.w();
    u32 h = a.w();
    if ((h & 3) != 3) return;
    u32 i = (h >> 2) - 1;
    jobject o;
    {
        std::lock_guard<std::mutex> lk(g_ref_lock);
        if (i >= g_weaks.size()) return;
        o = g_weaks[i];
        g_weaks[i] = nullptr;
        g_weaks_free.push_back(i);
    }
    if (o) E()->DeleteWeakGlobalRef(o);
}
void j_NewDirectByteBuffer(GuestThread &t) {
    Args a(t); a.w();
    u32 addr = a.w();
    jlong cap = (jlong)a.d();
    t.ret32(to_guest(E()->NewDirectByteBuffer(g2h(addr), cap)));
}
void j_GetDirectBufferAddress(GuestThread &t) {
    Args a(t); a.w();
    jobject buf = a.o();
    JNIEnv *e = E();
    void *p = e->GetDirectBufferAddress(buf);
    if (!p) return t.ret32(0);
    if (is_guest_ptr(p)) return t.ret32(h2g(p));
    // Buffer lives outside guest memory: hand out a guest mirror kept in sync around native calls.
    std::lock_guard<std::mutex> lk(g_mirror_lock);
    auto it = g_mirrors.find(p);
    if (it == g_mirrors.end()) {
        u32 size = (u32)e->GetDirectBufferCapacity(buf);
        u32 g = mem::map(size ? size : 1);
        memcpy(g2h(g), p, size);
        // Without the reference, a collected buffer's memory would be reused by other Java
        // objects while the mirror kept being copied back into it (heap corruption).
        it = g_mirrors.emplace(p, Mirror{p, g, size, e->NewGlobalRef(buf)}).first;
        LOGI("mirroring direct buffer %p (%u bytes) at guest 0x%08x", p, size, g);
    }
    t.ret32(it->second.guest);
}
void j_GetDirectBufferCapacity(GuestThread &t) { Args a(t); a.w(); t.ret64((u64)E()->GetDirectBufferCapacity(a.o())); }
void j_GetObjectRefType(GuestThread &t) { Args a(t); a.w(); t.ret32(a.w() & 3); }

void j_unimplemented(GuestThread &t) {
    cpu::dump_state(t);
    fatal("unimplemented JNI function %s", cpu::thunk_name(t.cur_svc));
}

// ------------------------------------------------------------------ JavaVM
void vm_DestroyJavaVM(GuestThread &t) { t.ret32((u32)JNI_ERR); }
void vm_AttachCurrentThread(GuestThread &t) {
    Args a(t); a.w();
    u32 penv = a.w();
    host_env();
    wr<u32>(penv, guest_env());
    t.ret32(JNI_OK);
}
void vm_DetachCurrentThread(GuestThread &t) {
    if (t_state.env && t_state.attached_by_us) {
        g_vm->DetachCurrentThread();
        t_state.env = nullptr;
        t_state.attached_by_us = false;
    }
    t.ret32(JNI_OK);
}
void vm_GetEnv(GuestThread &t) {
    Args a(t); a.w();
    u32 penv = a.w();
    JNIEnv *e = nullptr;
    if (g_vm->GetEnv((void **)&e, JNI_VERSION_1_6) != JNI_OK) {
        wr<u32>(penv, 0);
        return t.ret32((u32)JNI_EDETACHED);
    }
    t_state.env = e;
    wr<u32>(penv, guest_env());
    t.ret32(JNI_OK);
}

struct Entry {
    const char *name;
    ThunkFn fn;
};

#define CALL3(Name, K, KIND)                                                    \
    {"Call" Name "Method", jni_call<K, 0, KIND>}, {"Call" Name "MethodV", jni_call<K, 1, KIND>}, \
    {"Call" Name "MethodA", jni_call<K, 2, KIND>}
#define CALLS(Prefix, KIND)                                                                                   \
    CALL3(Prefix "Object", 'L', KIND), CALL3(Prefix "Boolean", 'Z', KIND), CALL3(Prefix "Byte", 'B', KIND),     \
    CALL3(Prefix "Char", 'C', KIND), CALL3(Prefix "Short", 'S', KIND), CALL3(Prefix "Int", 'I', KIND),          \
    CALL3(Prefix "Long", 'J', KIND), CALL3(Prefix "Float", 'F', KIND), CALL3(Prefix "Double", 'D', KIND),       \
    CALL3(Prefix "Void", 'V', KIND)
#define FIELDS(Op, fn, STATIC)                                                                              \
    {Op "ObjectField", fn<'L', STATIC>}, {Op "BooleanField", fn<'Z', STATIC>}, {Op "ByteField", fn<'B', STATIC>}, \
    {Op "CharField", fn<'C', STATIC>}, {Op "ShortField", fn<'S', STATIC>}, {Op "IntField", fn<'I', STATIC>},       \
    {Op "LongField", fn<'J', STATIC>}, {Op "FloatField", fn<'F', STATIC>}, {Op "DoubleField", fn<'D', STATIC>}
#define PRIMS(Pre, Post, fn)                                                                                  \
    {Pre "Boolean" Post, fn<'Z'>}, {Pre "Byte" Post, fn<'B'>}, {Pre "Char" Post, fn<'C'>},                    \
    {Pre "Short" Post, fn<'S'>}, {Pre "Int" Post, fn<'I'>}, {Pre "Long" Post, fn<'J'>},                       \
    {Pre "Float" Post, fn<'F'>}, {Pre "Double" Post, fn<'D'>}

// Order matches JNINativeInterface exactly (233 entries).
const Entry kEnvTable[] = {
    {"reserved0", nullptr}, {"reserved1", nullptr}, {"reserved2", nullptr}, {"reserved3", nullptr},
    {"GetVersion", j_GetVersion}, {"DefineClass", nullptr}, {"FindClass", j_FindClass},
    {"FromReflectedMethod", nullptr}, {"FromReflectedField", nullptr}, {"ToReflectedMethod", nullptr},
    {"GetSuperclass", j_GetSuperclass}, {"IsAssignableFrom", j_IsAssignableFrom}, {"ToReflectedField", nullptr},
    {"Throw", j_Throw}, {"ThrowNew", j_ThrowNew}, {"ExceptionOccurred", j_ExceptionOccurred},
    {"ExceptionDescribe", j_ExceptionDescribe}, {"ExceptionClear", j_ExceptionClear}, {"FatalError", j_FatalError},
    {"PushLocalFrame", j_PushLocalFrame}, {"PopLocalFrame", j_PopLocalFrame}, {"NewGlobalRef", j_NewGlobalRef},
    {"DeleteGlobalRef", j_DeleteGlobalRef}, {"DeleteLocalRef", j_DeleteLocalRef}, {"IsSameObject", j_IsSameObject},
    {"NewLocalRef", j_NewLocalRef}, {"EnsureLocalCapacity", j_EnsureLocalCapacity}, {"AllocObject", j_AllocObject},
    {"NewObject", jni_new_object<0>}, {"NewObjectV", jni_new_object<1>}, {"NewObjectA", jni_new_object<2>},
    {"GetObjectClass", j_GetObjectClass}, {"IsInstanceOf", j_IsInstanceOf}, {"GetMethodID", j_GetMethodID<false>},
    CALLS("", 0),
    CALLS("Nonvirtual", 1),
    {"GetFieldID", j_GetFieldID<false>},
    FIELDS("Get", jni_get_field, false),
    FIELDS("Set", jni_set_field, false),
    {"GetStaticMethodID", j_GetMethodID<true>},
    CALLS("Static", 2),
    {"GetStaticFieldID", j_GetFieldID<true>},
    FIELDS("GetStatic", jni_get_field, true),
    FIELDS("SetStatic", jni_set_field, true),
    {"NewString", j_NewString}, {"GetStringLength", j_GetStringLength}, {"GetStringChars", j_GetStringChars},
    {"ReleaseStringChars", j_ReleaseStringChars}, {"NewStringUTF", j_NewStringUTF},
    {"GetStringUTFLength", j_GetStringUTFLength}, {"GetStringUTFChars", j_GetStringUTFChars},
    {"ReleaseStringUTFChars", j_ReleaseStringUTFChars}, {"GetArrayLength", j_GetArrayLength},
    {"NewObjectArray", j_NewObjectArray}, {"GetObjectArrayElement", j_GetObjectArrayElement},
    {"SetObjectArrayElement", j_SetObjectArrayElement},
    PRIMS("New", "Array", jni_new_array),
    PRIMS("Get", "ArrayElements", jni_get_elements),
    PRIMS("Release", "ArrayElements", jni_release_elements),
    PRIMS("Get", "ArrayRegion", jni_get_region),
    PRIMS("Set", "ArrayRegion", jni_set_region),
    {"RegisterNatives", j_RegisterNatives}, {"UnregisterNatives", j_UnregisterNatives},
    {"MonitorEnter", j_MonitorEnter}, {"MonitorExit", j_MonitorExit}, {"GetJavaVM", j_GetJavaVM},
    {"GetStringRegion", j_GetStringRegion}, {"GetStringUTFRegion", j_GetStringUTFRegion},
    {"GetPrimitiveArrayCritical", j_GetPrimitiveArrayCritical},
    {"ReleasePrimitiveArrayCritical", j_ReleasePrimitiveArrayCritical},
    {"GetStringCritical", j_GetStringChars}, {"ReleaseStringCritical", j_ReleaseStringChars},
    {"NewWeakGlobalRef", j_NewWeakGlobalRef}, {"DeleteWeakGlobalRef", j_DeleteWeakGlobalRef},
    {"ExceptionCheck", j_ExceptionCheck}, {"NewDirectByteBuffer", j_NewDirectByteBuffer},
    {"GetDirectBufferAddress", j_GetDirectBufferAddress}, {"GetDirectBufferCapacity", j_GetDirectBufferCapacity},
    {"GetObjectRefType", j_GetObjectRefType},
};
static_assert(sizeof(kEnvTable) / sizeof(kEnvTable[0]) == 233, "JNINativeInterface has 233 entries");

const Entry kVmTable[] = {
    {"vm_reserved0", nullptr}, {"vm_reserved1", nullptr}, {"vm_reserved2", nullptr},
    {"DestroyJavaVM", vm_DestroyJavaVM}, {"AttachCurrentThread", vm_AttachCurrentThread},
    {"DetachCurrentThread", vm_DetachCurrentThread}, {"GetEnv", vm_GetEnv},
    {"AttachCurrentThreadAsDaemon", vm_AttachCurrentThread},
};

u32 build_table(const Entry *e, size_t n) {
    u32 table = mem::halloc((u32)n * 4, 16);
    for (size_t i = 0; i < n; i++) {
        if (!e[i].name || strncmp(e[i].name, "reserved", 8) == 0 || strncmp(e[i].name, "vm_reserved", 11) == 0) {
            wr<u32>(table + (u32)i * 4, 0);
            continue;
        }
        std::string name = std::string("JNI::") + e[i].name;
        wr<u32>(table + (u32)i * 4, cpu::add_thunk(name.c_str(), e[i].fn ? e[i].fn : j_unimplemented));
    }
    return table;
}
}  // namespace

void init(JavaVM *vm) {
    g_vm = vm;
    g_env_table = build_table(kEnvTable, sizeof(kEnvTable) / sizeof(kEnvTable[0]));
    g_vm_table = build_table(kVmTable, sizeof(kVmTable) / sizeof(kVmTable[0]));
    g_gvm = mem::halloc(8);
    wr<u32>(g_gvm, g_vm_table);
}

JNIEnv *host_env() {
    if (t_state.env) return t_state.env;
    JNIEnv *e = nullptr;
    if (g_vm->GetEnv((void **)&e, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) fatal("AttachCurrentThread failed");
        t_state.attached_by_us = true;
    }
    t_state.env = e;
    return e;
}

u32 guest_vm() { return g_gvm; }
u32 guest_env() {
    if (!t_state.genv) {
        t_state.genv = mem::halloc(8);
        wr<u32>(t_state.genv, g_env_table);
    }
    return t_state.genv;
}

u32 to_guest(jobject o) {
    if (!o) return 0;
    t_state.locals.push_back(o);
    return ((u32)t_state.locals.size() << 2) | 1;
}

jobject to_host(u32 h) {
    if (!h) return nullptr;
    u32 i = (h >> 2) - 1;
    switch (h & 3) {
    case 1:
        if (i < t_state.locals.size()) return t_state.locals[i];
        break;
    case 2: {
        std::lock_guard<std::mutex> lk(g_ref_lock);
        if (i < g_globals.size()) return g_globals[i];
        break;
    }
    case 3: {
        std::lock_guard<std::mutex> lk(g_ref_lock);
        if (i < g_weaks.size()) return g_weaks[i];
        break;
    }
    default: break;
    }
    LOGE("invalid guest JNI reference 0x%08x", h);
    return nullptr;
}

NativeFrame::NativeFrame() : mark(t_state.locals.size()) {
    std::lock_guard<std::mutex> lk(g_mirror_lock);
    for (auto &kv : g_mirrors) memcpy(g2h(kv.second.guest), kv.second.host, kv.second.size);
}
NativeFrame::~NativeFrame() {
    {
        std::lock_guard<std::mutex> lk(g_mirror_lock);
        for (auto &kv : g_mirrors) memcpy(kv.second.host, g2h(kv.second.guest), kv.second.size);
    }
    t_state.locals.resize(mark);
}
}  // namespace jni
