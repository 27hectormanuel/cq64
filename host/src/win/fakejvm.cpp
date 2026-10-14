// SPDX-License-Identifier: CC0-1.0
// Windows build: the stand-in Java VM (see fakejvm.h).
#include "fakejvm.h"

#include "../common.h"

#include <android/native_window_jni.h>
#include <SDL2/SDL.h>
#include <zlib.h>

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fakejvm {
namespace {
struct Class;
struct Field;

struct Obj {
    Class *cls = nullptr;
    std::unordered_map<const Field *, jvalue> fields;
    virtual ~Obj() = default;
};
struct Str : Obj {
    std::string utf8;
};
struct Arr : Obj {
    char kind = 'B';  // element type: Z B C S I J F D, or L for objects
    jsize n = 0;
    std::vector<uint8_t> bytes;
    std::vector<Obj *> objs;
};
struct Buffer : Obj {  // java.nio.ByteBuffer (direct)
    void *addr = nullptr;
    jlong cap = 0;
};
struct Stream : Obj {  // an asset as java.io.InputStream / ReadableByteChannel
    std::vector<uint8_t> data;
    size_t pos = 0;
};
struct AssetFd : Obj {  // android.content.res.AssetFileDescriptor
    int fd = -1;
    jlong start = 0, length = 0;
};

using Impl = std::function<jvalue(Obj *self, const jvalue *args)>;
struct Method {
    Class *cls;
    std::string name, sig;
    bool is_static;
    Impl fn;
    bool stub = false;
};
struct Field {
    Class *cls;
    std::string name, sig;
    bool is_static;
};
struct Class : Obj {
    std::string name;
    Class *super = nullptr;
    std::map<std::string, std::unique_ptr<Method>> methods;  // name + sig
    std::map<std::string, std::unique_ptr<Field>> fieldids;   // name + sig
    std::map<std::string, void *> natives;                    // name -> registered function
};

std::recursive_mutex g_lock;
std::map<std::string, std::unique_ptr<Class>> g_classes;
thread_local Obj *t_exception;

JNIEnv g_env;
JavaVM g_vm;
JNINativeInterface g_iface;
JNIInvokeInterface g_invoke;

Obj *g_activity, *g_assets, *g_surface;
void *g_hwnd;
std::atomic<int> g_width{1280}, g_height{720};

jvalue V() { return jvalue{}; }
jvalue VI(jint v) { jvalue r{}; r.i = v; return r; }
jvalue VZ(bool v) { jvalue r{}; r.z = v ? JNI_TRUE : JNI_FALSE; return r; }
jvalue VJ(jlong v) { jvalue r{}; r.j = v; return r; }
jvalue VL(Obj *o) { jvalue r{}; r.l = (jobject)o; return r; }

Obj *O(jobject o) { return (Obj *)o; }

Class *cls(const std::string &name, const char *super = "java/lang/Object") {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    auto &c = g_classes[name];
    if (!c) {
        c = std::make_unique<Class>();
        c->name = name;
        if (name != "java/lang/Object") c->super = cls(super ? super : "java/lang/Object");
        c->cls = name == "java/lang/Class" ? c.get() : cls("java/lang/Class");
    }
    return c.get();
}

void def(const char *c, const char *name, const char *sig, Impl fn, bool is_static = false) {
    Class *k = cls(c);
    k->methods[std::string(name) + sig] = std::make_unique<Method>(Method{k, name, sig, is_static, std::move(fn)});
}
void defs(const char *c, const char *name, const char *sig, Impl fn) { def(c, name, sig, std::move(fn), true); }

Str *str(const std::string &s) {
    auto *o = new Str;
    o->cls = cls("java/lang/String");
    o->utf8 = s;
    return o;
}
std::string sval(jobject o) { return o ? ((Str *)o)->utf8 : std::string(); }
std::string sval(const jvalue &v) { return sval(v.l); }

template <class T> T *make(const char *c) {
    auto *o = new T;
    o->cls = cls(c);
    return o;
}

void throw_new(const char *c, const std::string &msg) {
    Obj *e = make<Obj>(c);
    LOGI("fakejvm: throwing %s: %s", c, msg.c_str());
    t_exception = e;
}

bool is_a(Class *k, Class *of) {
    for (; k; k = k->super)
        if (k == of) return true;
    return false;
}

Method *find_method(Class *k, const std::string &key) {
    for (Class *c = k; c; c = c->super) {
        auto it = c->methods.find(key);
        if (it != c->methods.end()) return it->second.get();
    }
    return nullptr;
}

// ---- UTF-8 <-> UTF-16 (strings are kept as UTF-8)
std::u16string to16(const std::string &s) {
    std::u16string r;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        int len;
        if (c < 0x80) cp = c, len = 1;
        else if ((c >> 5) == 6) cp = c & 0x1f, len = 2;
        else if ((c >> 4) == 14) cp = c & 0x0f, len = 3;
        else cp = c & 0x07, len = 4;
        for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3f);
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            r += (char16_t)(0xd800 + (cp >> 10));
            r += (char16_t)(0xdc00 + (cp & 0x3ff));
        } else {
            r += (char16_t)cp;
        }
    }
    return r;
}
std::string to8(const char16_t *s, size_t n) {
    std::string r;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n) cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00);
        if (cp < 0x80) r += (char)cp;
        else if (cp < 0x800) r += (char)(0xc0 | (cp >> 6)), r += (char)(0x80 | (cp & 0x3f));
        else if (cp < 0x10000)
            r += (char)(0xe0 | (cp >> 12)), r += (char)(0x80 | ((cp >> 6) & 0x3f)), r += (char)(0x80 | (cp & 0x3f));
        else
            r += (char)(0xf0 | (cp >> 18)), r += (char)(0x80 | ((cp >> 12) & 0x3f)),
                r += (char)(0x80 | ((cp >> 6) & 0x3f)), r += (char)(0x80 | (cp & 0x3f));
    }
    return r;
}

// ---- the APK's assets (a zip): central directory of "assets/..." entries
struct ZipEntry {
    uint32_t local_offset, csize, usize;
    uint16_t method;
};
std::map<std::string, ZipEntry> g_assets_dir;

void read_zip_dir(const std::string &apk) {
    FILE *f = fopen(apk.c_str(), "rb");
    if (!f) fatal("cannot open the APK %s", apk.c_str());
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    long long tail = std::min<long long>(size, 65557);
    std::vector<uint8_t> t(tail);
    _fseeki64(f, size - tail, SEEK_SET);
    if (fread(t.data(), 1, tail, f) != (size_t)tail) fatal("cannot read the APK");
    long long eocd = -1;
    for (long long i = tail - 22; i >= 0; i--)
        if (t[i] == 'P' && t[i + 1] == 'K' && t[i + 2] == 5 && t[i + 3] == 6) {
            eocd = i;
            break;
        }
    if (eocd < 0) fatal("%s is not a zip/APK", apk.c_str());
    uint32_t cd_size, cd_off;
    memcpy(&cd_size, &t[eocd + 12], 4);
    memcpy(&cd_off, &t[eocd + 16], 4);
    std::vector<uint8_t> cd(cd_size);
    _fseeki64(f, cd_off, SEEK_SET);
    if (fread(cd.data(), 1, cd_size, f) != cd_size) fatal("cannot read the APK's directory");
    fclose(f);
    for (size_t p = 0; p + 46 <= cd.size();) {
        uint16_t method, nl, el, cl;
        uint32_t csize, usize, off;
        memcpy(&method, &cd[p + 10], 2);
        memcpy(&csize, &cd[p + 20], 4);
        memcpy(&usize, &cd[p + 24], 4);
        memcpy(&nl, &cd[p + 28], 2);
        memcpy(&el, &cd[p + 30], 2);
        memcpy(&cl, &cd[p + 32], 2);
        memcpy(&off, &cd[p + 42], 4);
        std::string name((const char *)&cd[p + 46], nl);
        if (name.rfind("assets/", 0) == 0) g_assets_dir[name.substr(7)] = ZipEntry{off, csize, usize, method};
        p += 46 + nl + el + cl;
    }
    LOGI("fakejvm: %zu assets in %s", g_assets_dir.size(), apk.c_str());
}

// Offset of an entry's data (after its local header).
long long data_offset(int fd, const ZipEntry &e) {
    uint8_t h[30];
    if (pread(fd, h, 30, e.local_offset) != 30) return -1;
    uint16_t nl, el;
    memcpy(&nl, &h[26], 2);
    memcpy(&el, &h[28], 2);
    return (long long)e.local_offset + 30 + nl + el;
}

const ZipEntry *asset(const std::string &name) {
    std::string n = name;
    while (n.rfind("./", 0) == 0) n = n.substr(2);
    auto it = g_assets_dir.find(n);
    return it == g_assets_dir.end() ? nullptr : &it->second;
}

// ---- the Java classes the game uses
void define_classes() {
    for (const char *c : {"java/lang/Object", "java/lang/Class", "java/lang/String", "java/lang/Throwable"}) cls(c);
    cls("java/lang/Exception", "java/lang/Throwable");
    cls("java/io/IOException", "java/lang/Exception");
    cls("java/io/FileNotFoundException", "java/io/IOException");
    cls("android/content/Context");
    cls("android/app/Activity", "android/content/Context");
    cls("org/libsdl/app/SDLActivity", "android/app/Activity");
    cls("com/doublefine/cq/CqActivity", "org/libsdl/app/SDLActivity");
    cls("java/io/InputStream");
    cls("java/nio/ByteBuffer");

    def("java/lang/Object", "toString", "()Ljava/lang/String;", [](Obj *self, const jvalue *) {
        if (self && self->cls == cls("java/lang/String")) return VL(self);
        return VL(str(self ? self->cls->name : "null"));
    });
    def("java/lang/Object", "hashCode", "()I", [](Obj *self, const jvalue *) { return VI((jint)(intptr_t)self); });
    def("java/lang/String", "length", "()I", [](Obj *self, const jvalue *) { return VI((jint)to16(((Str *)self)->utf8).size()); });

    // Context / Activity
    const char *C = "android/content/Context";
    def(C, "getAssets", "()Landroid/content/res/AssetManager;", [](Obj *, const jvalue *) { return VL(g_assets); });
    def(C, "getFilesDir", "()Ljava/io/File;", [](Obj *, const jvalue *) {
        Obj *f = make<Obj>("java/io/File");
        f->fields[nullptr] = VL(str("/data/data/com.doublefine.cq/files"));
        return VL(f);
    });
    def(C, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", [](Obj *, const jvalue *) {
        Obj *f = make<Obj>("java/io/File");
        f->fields[nullptr] = VL(str("/sdcard/Android/data/com.doublefine.cq/files"));
        return VL(f);
    });
    def(C, "getPackageName", "()Ljava/lang/String;", [](Obj *, const jvalue *) { return VL(str("com.doublefine.cq")); });
    def(C, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", [](Obj *, const jvalue *a) {
        if (sval(a[0]) == "clipboard") return VL(make<Obj>("android/content/ClipboardManager"));
        return V();
    });
    def(C, "registerReceiver",
        "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",
        [](Obj *, const jvalue *) { return VL(make<Obj>("android/content/Intent")); });
    // A desktop: always plugged in and full.
    def("android/content/Intent", "getIntExtra", "(Ljava/lang/String;I)I", [](Obj *, const jvalue *a) {
        std::string k = sval(a[0]);
        if (k == "level" || k == "scale") return VI(100);
        if (k == "plugged") return VI(1);
        if (k == "status") return VI(5);
        return VI(a[1].i);
    });
    def("android/content/Intent", "getBooleanExtra", "(Ljava/lang/String;Z)Z", [](Obj *, const jvalue *) { return VZ(true); });
    def("android/content/IntentFilter", "<init>", "(Ljava/lang/String;)V", [](Obj *, const jvalue *) { return V(); });
    def("android/app/Activity", "finish", "()V", [](Obj *, const jvalue *) {
        LOGI("fakejvm: Activity.finish(): quitting");
        SDL_Event e{};
        e.type = SDL_QUIT;
        SDL_PushEvent(&e);
        return V();
    });
    def("com/doublefine/cq/CqActivity", "isPurchased", "()Z", [](Obj *, const jvalue *) { return VZ(true); });
    def("com/doublefine/cq/CqActivity", "startTrialUpgrade", "()V", [](Obj *, const jvalue *) { return V(); });

    // java.io.File
    def("java/io/File", "getAbsolutePath", "()Ljava/lang/String;", [](Obj *self, const jvalue *) { return self->fields[nullptr]; });
    def("java/io/File", "getPath", "()Ljava/lang/String;", [](Obj *self, const jvalue *) { return self->fields[nullptr]; });

    // Clipboard
    const char *CB = "android/content/ClipboardManager";
    def(CB, "setText", "(Ljava/lang/CharSequence;)V", [](Obj *, const jvalue *a) {
        SDL_SetClipboardText(sval(a[0]).c_str());
        return V();
    });
    def(CB, "getText", "()Ljava/lang/CharSequence;", [](Obj *, const jvalue *) {
        char *t = SDL_GetClipboardText();
        Str *s = str(t ? t : "");
        SDL_free(t);
        return VL(s);
    });
    def(CB, "hasText", "()Z", [](Obj *, const jvalue *) { return VZ(SDL_HasClipboardText()); });

    // SDLActivity's static methods (SDL_android.c)
    const char *S = "org/libsdl/app/SDLActivity";
    defs(S, "getContext", "()Landroid/content/Context;", [](Obj *, const jvalue *) { return VL(g_activity); });
    defs(S, "getNativeSurface", "()Landroid/view/Surface;", [](Obj *, const jvalue *) { return VL(g_surface); });
    defs(S, "flipBuffers", "()V", [](Obj *, const jvalue *) { return V(); });
    defs(S, "setActivityTitle", "(Ljava/lang/String;)Z", [](Obj *, const jvalue *) { return VZ(true); });
    defs(S, "sendMessage", "(II)Z", [](Obj *, const jvalue *) { return VZ(true); });
    defs(S, "showTextInput", "(IIII)Z", [](Obj *, const jvalue *) { return VZ(true); });
    // SDL audio (the game plays its sound through FMOD; SDL's own output stays silent).
    defs(S, "audioInit", "(IZZI)I", [](Obj *, const jvalue *a) {
        LOGI("fakejvm: SDL audioInit(%d Hz, 16-bit %d, stereo %d, %d frames)", a[0].i, a[1].z, a[2].z, a[3].i);
        return VI(0);
    });
    defs(S, "audioWriteShortBuffer", "([S)V", [](Obj *, const jvalue *a) {
        Arr *b = (Arr *)a[0].l;
        SDL_Delay(b ? (Uint32)(b->n / 2 * 1000 / 44100) : 10);
        return V();
    });
    defs(S, "audioWriteByteBuffer", "([B)V", [](Obj *, const jvalue *a) {
        Arr *b = (Arr *)a[0].l;
        SDL_Delay(b ? (Uint32)(b->n / 2 * 1000 / 44100) : 10);
        return V();
    });
    defs(S, "audioQuit", "()V", [](Obj *, const jvalue *) { return V(); });

    // Assets
    const char *AM = "android/content/res/AssetManager";
    def(AM, "openFd", "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;", [](Obj *, const jvalue *a) {
        std::string name = sval(a[0]);
        const ZipEntry *e = asset(name);
        if (!e || e->method != 0) {
            throw_new("java/io/FileNotFoundException", name + (e ? " (compressed)" : ""));
            return V();
        }
        int fd = _open(win::apk_path().c_str(), _O_RDONLY | _O_BINARY);
        long long off = fd < 0 ? -1 : data_offset(fd, *e);
        if (off < 0) {
            if (fd >= 0) _close(fd);
            throw_new("java/io/FileNotFoundException", name);
            return V();
        }
        auto *d = make<AssetFd>("android/content/res/AssetFileDescriptor");
        d->fd = fd;
        d->start = off;
        d->length = e->usize;
        return VL(d);
    });
    def(AM, "open", "(Ljava/lang/String;I)Ljava/io/InputStream;", [](Obj *, const jvalue *a) {
        std::string name = sval(a[0]);
        const ZipEntry *e = asset(name);
        int fd = e ? _open(win::apk_path().c_str(), _O_RDONLY | _O_BINARY) : -1;
        long long off = fd < 0 ? -1 : data_offset(fd, *e);
        auto *s = make<Stream>("java/io/InputStream");
        bool ok = off >= 0;
        if (ok) {
            std::vector<uint8_t> raw(e->csize);
            ok = pread(fd, raw.data(), raw.size(), off) == (ssize_t)raw.size();
            if (ok && e->method == 0) {
                s->data = std::move(raw);
            } else if (ok && e->method == 8) {
                s->data.resize(e->usize);
                z_stream z{};
                inflateInit2(&z, -MAX_WBITS);
                z.next_in = raw.data();
                z.avail_in = (uInt)raw.size();
                z.next_out = s->data.data();
                z.avail_out = (uInt)s->data.size();
                ok = inflate(&z, Z_FINISH) == Z_STREAM_END;
                inflateEnd(&z);
            } else {
                ok = false;
            }
        }
        if (fd >= 0) _close(fd);
        if (!ok) {
            delete s;
            throw_new("java/io/FileNotFoundException", name);
            return V();
        }
        return VL(s);
    });
    def(AM, "list", "(Ljava/lang/String;)[Ljava/lang/String;", [](Obj *, const jvalue *) {
        auto *a = make<Arr>("[Ljava/lang/String;");
        a->kind = 'L';
        return VL(a);
    });
    const char *AFD = "android/content/res/AssetFileDescriptor";
    def(AFD, "getStartOffset", "()J", [](Obj *self, const jvalue *) { return VJ(((AssetFd *)self)->start); });
    def(AFD, "getDeclaredLength", "()J", [](Obj *self, const jvalue *) { return VJ(((AssetFd *)self)->length); });
    def(AFD, "getLength", "()J", [](Obj *self, const jvalue *) { return VJ(((AssetFd *)self)->length); });
    def(AFD, "getFileDescriptor", "()Ljava/io/FileDescriptor;", [](Obj *self, const jvalue *) {
        Obj *fdo = make<Obj>("java/io/FileDescriptor");
        Class *k = fdo->cls;
        auto &fid = k->fieldids["descriptorI"];
        if (!fid) fid = std::make_unique<Field>(Field{k, "descriptor", "I", false});
        fdo->fields[fid.get()] = VI(((AssetFd *)self)->fd);
        return VL(fdo);
    });
    def(AFD, "close", "()V", [](Obj *self, const jvalue *) {
        auto *d = (AssetFd *)self;
        if (d->fd >= 0) _close(d->fd);
        d->fd = -1;
        return V();
    });
    const char *IS = "java/io/InputStream";
    def(IS, "available", "()I", [](Obj *self, const jvalue *) {
        auto *s = (Stream *)self;
        return VI((jint)(s->data.size() - s->pos));
    });
    def(IS, "close", "()V", [](Obj *, const jvalue *) { return V(); });
    def(IS, "skip", "(J)J", [](Obj *self, const jvalue *a) {
        auto *s = (Stream *)self;
        size_t n = (size_t)std::min<jlong>(a[0].j, (jlong)(s->data.size() - s->pos));
        s->pos += n;
        return VJ((jlong)n);
    });
    defs("java/nio/channels/Channels", "newChannel",
         "(Ljava/io/InputStream;)Ljava/nio/channels/ReadableByteChannel;", [](Obj *, const jvalue *a) {
             Obj *s = O(a[0].l);
             return VL(s);  // the stream doubles as its channel
         });
    // ReadableByteChannel.read(ByteBuffer) on the stream (the channel is the stream itself).
    def(IS, "read", "(Ljava/nio/ByteBuffer;)I", [](Obj *self, const jvalue *a) {
        auto *s = (Stream *)self;
        auto *b = (Buffer *)a[0].l;
        if (s->pos >= s->data.size()) return VI(-1);
        size_t n = std::min<size_t>((size_t)b->cap, s->data.size() - s->pos);
        memcpy(b->addr, s->data.data() + s->pos, n);
        s->pos += n;
        return VI((jint)n);
    });
    cls("java/nio/channels/ReadableByteChannel");
}

// ---- JNIEnv functions
#define ENV JNIEnv *
Method *M(jmethodID m) { return (Method *)m; }

jvalue invoke(jobject obj, jmethodID mid, const jvalue *args) {
    Method *m = M(mid);
    if (!m) fatal("fakejvm: call through a null method ID");
    return m->fn(O(obj), args);
}

// Builds the argument array of a variadic call from the method's signature.
std::vector<jvalue> va_args(jmethodID mid, va_list ap) {
    std::vector<jvalue> v;
    const char *p = M(mid)->sig.c_str() + 1;
    while (*p && *p != ')') {
        jvalue x{};
        char c = *p;
        if (c == 'L' || c == '[') {
            x.l = va_arg(ap, jobject);
            while (*p == '[') p++;
            if (*p == 'L') p = strchr(p, ';');
            p++;
        } else {
            if (c == 'J') x.j = va_arg(ap, jlong);
            else if (c == 'F') x.f = (jfloat)va_arg(ap, double);
            else if (c == 'D') x.d = va_arg(ap, double);
            else x.i = va_arg(ap, jint);
            p++;
        }
        v.push_back(x);
    }
    return v;
}

jint GetVersion(ENV) { return JNI_VERSION_1_6; }
jclass FindClass(ENV, const char *name) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!g_classes.count(name)) LOGI("fakejvm: FindClass(%s): new empty class", name);
    return (jclass)cls(name);
}
jclass GetSuperclass(ENV, jclass c) { return (jclass)((Class *)c)->super; }
jboolean IsAssignableFrom(ENV, jclass a, jclass b) { return is_a((Class *)a, (Class *)b); }
jint Throw(ENV, jthrowable t) { t_exception = O(t); return 0; }
jint ThrowNew(ENV, jclass c, const char *msg) {
    throw_new(((Class *)c)->name.c_str(), msg ? msg : "");
    return 0;
}
jthrowable ExceptionOccurred(ENV) { return (jthrowable)t_exception; }
void ExceptionDescribe(ENV) {
    if (t_exception) LOGW("fakejvm: pending exception %s", t_exception->cls->name.c_str());
}
void ExceptionClear(ENV) { t_exception = nullptr; }
jboolean ExceptionCheck(ENV) { return t_exception != nullptr; }
void FatalError(ENV, const char *msg) { fatal("JNI FatalError: %s", msg); }
jint PushLocalFrame(ENV, jint) { return 0; }
jobject PopLocalFrame(ENV, jobject r) { return r; }
jobject NewGlobalRef(ENV, jobject o) { return o; }
void DeleteGlobalRef(ENV, jobject) {}
void DeleteLocalRef(ENV, jobject) {}
jboolean IsSameObject(ENV, jobject a, jobject b) { return a == b; }
jobject NewLocalRef(ENV, jobject o) { return o; }
jint EnsureLocalCapacity(ENV, jint) { return 0; }
jobject AllocObject(ENV, jclass c) {
    Obj *o = new Obj;
    o->cls = (Class *)c;
    return (jobject)o;
}
jobject NewObjectA(ENV e, jclass c, jmethodID ctor, const jvalue *args) {
    jobject o = AllocObject(e, c);
    if (ctor) invoke(o, ctor, args);
    return o;
}
jobject NewObjectV(ENV e, jclass c, jmethodID ctor, va_list ap) {
    auto v = va_args(ctor, ap);
    return NewObjectA(e, c, ctor, v.data());
}
jobject NewObject(ENV e, jclass c, jmethodID ctor, ...) {
    va_list ap;
    va_start(ap, ctor);
    jobject o = NewObjectV(e, c, ctor, ap);
    va_end(ap);
    return o;
}
jclass GetObjectClass(ENV, jobject o) { return o ? (jclass)O(o)->cls : nullptr; }
jboolean IsInstanceOf(ENV, jobject o, jclass c) { return !o || is_a(O(o)->cls, (Class *)c); }

jmethodID get_method(jclass c, const char *name, const char *sig, bool is_static) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Class *k = (Class *)c;
    std::string key = std::string(name) + sig;
    if (Method *m = find_method(k, key)) return (jmethodID)m;
    // Not implemented: a stub that logs once and returns zero.
    auto stub = std::make_unique<Method>();
    Method *m = stub.get();
    *m = Method{k, name, sig, is_static, nullptr, true};
    m->fn = [m](Obj *, const jvalue *) {
        static std::set<Method *> seen;
        std::lock_guard<std::recursive_mutex> lk2(g_lock);
        if (seen.insert(m).second) LOGW("fakejvm: unimplemented %s.%s%s called", m->cls->name.c_str(), m->name.c_str(), m->sig.c_str());
        return V();
    };
    k->methods[key] = std::move(stub);
    return (jmethodID)m;
}
jmethodID GetMethodID(ENV, jclass c, const char *name, const char *sig) { return get_method(c, name, sig, false); }
jmethodID GetStaticMethodID(ENV, jclass c, const char *name, const char *sig) { return get_method(c, name, sig, true); }

jfieldID get_field(jclass c, const char *name, const char *sig, bool is_static) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Class *k = (Class *)c;
    std::string key = std::string(name) + sig;
    for (Class *p = k; p; p = p->super) {
        auto it = p->fieldids.find(key);
        if (it != p->fieldids.end()) return (jfieldID)it->second.get();
    }
    auto &f = k->fieldids[key];
    f = std::make_unique<Field>(Field{k, name, sig, is_static});
    return (jfieldID)f.get();
}
jfieldID GetFieldID(ENV, jclass c, const char *name, const char *sig) { return get_field(c, name, sig, false); }
jfieldID GetStaticFieldID(ENV, jclass c, const char *name, const char *sig) { return get_field(c, name, sig, true); }
jvalue &field(jobject o, jfieldID f) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    return O(o)->fields[(const Field *)f];
}

// Typed Call/Get/Set families
#define CALLS(T, Name, member)                                                                                    \
    T Call##Name##MethodA(ENV, jobject o, jmethodID m, const jvalue *a) { return invoke(o, m, a).member; }        \
    T Call##Name##MethodV(ENV, jobject o, jmethodID m, va_list ap) {                                              \
        auto v = va_args(m, ap);                                                                                  \
        return invoke(o, m, v.data()).member;                                                                     \
    }                                                                                                             \
    T Call##Name##Method(ENV e, jobject o, jmethodID m, ...) {                                                    \
        va_list ap;                                                                                               \
        va_start(ap, m);                                                                                          \
        T r = Call##Name##MethodV(e, o, m, ap);                                                                   \
        va_end(ap);                                                                                               \
        return r;                                                                                                 \
    }                                                                                                             \
    T CallNonvirtual##Name##MethodA(ENV, jobject o, jclass, jmethodID m, const jvalue *a) { return invoke(o, m, a).member; } \
    T CallStatic##Name##MethodA(ENV, jclass c, jmethodID m, const jvalue *a) { return invoke(c, m, a).member; }   \
    T CallStatic##Name##MethodV(ENV, jclass c, jmethodID m, va_list ap) {                                         \
        auto v = va_args(m, ap);                                                                                  \
        return invoke(c, m, v.data()).member;                                                                     \
    }                                                                                                             \
    T CallStatic##Name##Method(ENV e, jclass c, jmethodID m, ...) {                                               \
        va_list ap;                                                                                               \
        va_start(ap, m);                                                                                          \
        T r = CallStatic##Name##MethodV(e, c, m, ap);                                                             \
        va_end(ap);                                                                                               \
        return r;                                                                                                 \
    }                                                                                                             \
    T Get##Name##Field(ENV, jobject o, jfieldID f) { return field(o, f).member; }                                 \
    void Set##Name##Field(ENV, jobject o, jfieldID f, T v) { field(o, f).member = v; }                            \
    T GetStatic##Name##Field(ENV, jclass c, jfieldID f) { return field(c, f).member; }                            \
    void SetStatic##Name##Field(ENV, jclass c, jfieldID f, T v) { field(c, f).member = v; }
CALLS(jobject, Object, l)
CALLS(jboolean, Boolean, z)
CALLS(jbyte, Byte, b)
CALLS(jchar, Char, c)
CALLS(jshort, Short, s)
CALLS(jint, Int, i)
CALLS(jlong, Long, j)
CALLS(jfloat, Float, f)
CALLS(jdouble, Double, d)
#undef CALLS
void CallVoidMethodA(ENV, jobject o, jmethodID m, const jvalue *a) { invoke(o, m, a); }
void CallVoidMethodV(ENV, jobject o, jmethodID m, va_list ap) {
    auto v = va_args(m, ap);
    invoke(o, m, v.data());
}
void CallVoidMethod(ENV e, jobject o, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    CallVoidMethodV(e, o, m, ap);
    va_end(ap);
}
void CallNonvirtualVoidMethodA(ENV, jobject o, jclass, jmethodID m, const jvalue *a) { invoke(o, m, a); }
void CallStaticVoidMethodA(ENV, jclass c, jmethodID m, const jvalue *a) { invoke(c, m, a); }
void CallStaticVoidMethodV(ENV, jclass c, jmethodID m, va_list ap) {
    auto v = va_args(m, ap);
    invoke(c, m, v.data());
}
void CallStaticVoidMethod(ENV e, jclass c, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    CallStaticVoidMethodV(e, c, m, ap);
    va_end(ap);
}

// Strings
jstring NewString(ENV, const jchar *s, jsize n) { return (jstring)str(to8((const char16_t *)s, n)); }
jsize GetStringLength(ENV, jstring s) { return (jsize)to16(sval(s)).size(); }
const jchar *GetStringChars(ENV, jstring s, jboolean *copy) {
    std::u16string u = to16(sval(s));
    auto *r = (jchar *)malloc((u.size() + 1) * 2);
    memcpy(r, u.c_str(), (u.size() + 1) * 2);
    if (copy) *copy = JNI_TRUE;
    return r;
}
void ReleaseStringChars(ENV, jstring, const jchar *p) { free((void *)p); }
jstring NewStringUTF(ENV, const char *s) { return s ? (jstring)str(s) : nullptr; }
jsize GetStringUTFLength(ENV, jstring s) { return (jsize)sval(s).size(); }
const char *GetStringUTFChars(ENV, jstring s, jboolean *copy) {
    if (copy) *copy = JNI_TRUE;
    return strdup(sval(s).c_str());
}
void ReleaseStringUTFChars(ENV, jstring, const char *p) { free((void *)p); }
void GetStringRegion(ENV, jstring s, jsize start, jsize len, jchar *buf) {
    std::u16string u = to16(sval(s));
    memcpy(buf, u.data() + start, len * 2);
}
void GetStringUTFRegion(ENV, jstring s, jsize start, jsize len, char *buf) {
    std::u16string u = to16(sval(s));
    std::string r = to8(u.data() + start, len);
    memcpy(buf, r.c_str(), r.size() + 1);
}

// Arrays
int esize(char k) { return k == 'J' || k == 'D' ? 8 : k == 'I' || k == 'F' ? 4 : k == 'C' || k == 'S' ? 2 : 1; }
Arr *new_array(char kind, jsize n) {
    auto *a = new Arr;
    a->kind = kind;
    a->n = n;
    a->cls = cls(std::string("[") + kind);
    if (kind == 'L') a->objs.assign(n, nullptr);
    else a->bytes.assign((size_t)n * esize(kind), 0);
    return a;
}
jsize GetArrayLength(ENV, jarray a) { return ((Arr *)a)->n; }
jobjectArray NewObjectArray(ENV, jsize n, jclass, jobject init) {
    Arr *a = new_array('L', n);
    for (auto &o : a->objs) o = O(init);
    return (jobjectArray)a;
}
jobject GetObjectArrayElement(ENV, jobjectArray a, jsize i) { return (jobject)((Arr *)a)->objs.at(i); }
void SetObjectArrayElement(ENV, jobjectArray a, jsize i, jobject v) { ((Arr *)a)->objs.at(i) = O(v); }
#define ARRAYS(T, Name, K)                                                                                      \
    T##Array New##Name##Array(ENV, jsize n) { return (T##Array)new_array(K, n); }                               \
    T *Get##Name##ArrayElements(ENV, T##Array a, jboolean *copy) {                                               \
        if (copy) *copy = JNI_FALSE;                                                                             \
        return (T *)((Arr *)a)->bytes.data();                                                                    \
    }                                                                                                            \
    void Release##Name##ArrayElements(ENV, T##Array, T *, jint) {}                                              \
    void Get##Name##ArrayRegion(ENV, T##Array a, jsize s, jsize n, T *buf) {                                     \
        memcpy(buf, ((Arr *)a)->bytes.data() + (size_t)s * sizeof(T), (size_t)n * sizeof(T));                    \
    }                                                                                                            \
    void Set##Name##ArrayRegion(ENV, T##Array a, jsize s, jsize n, const T *buf) {                               \
        memcpy(((Arr *)a)->bytes.data() + (size_t)s * sizeof(T), buf, (size_t)n * sizeof(T));                    \
    }
ARRAYS(jboolean, Boolean, 'Z')
ARRAYS(jbyte, Byte, 'B')
ARRAYS(jchar, Char, 'C')
ARRAYS(jshort, Short, 'S')
ARRAYS(jint, Int, 'I')
ARRAYS(jlong, Long, 'J')
ARRAYS(jfloat, Float, 'F')
ARRAYS(jdouble, Double, 'D')
#undef ARRAYS
void *GetPrimitiveArrayCritical(ENV, jarray a, jboolean *copy) {
    if (copy) *copy = JNI_FALSE;
    return ((Arr *)a)->bytes.data();
}
void ReleasePrimitiveArrayCritical(ENV, jarray, void *, jint) {}

// Natives, monitors, VM, buffers, refs
jint RegisterNatives(ENV, jclass c, const JNINativeMethod *m, jint n) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    for (jint i = 0; i < n; i++) ((Class *)c)->natives[m[i].name] = m[i].fnPtr;
    return JNI_OK;
}
jint UnregisterNatives(ENV, jclass c) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ((Class *)c)->natives.clear();
    return JNI_OK;
}
jint MonitorEnter(ENV, jobject) { return 0; }
jint MonitorExit(ENV, jobject) { return 0; }
jint GetJavaVM(ENV, JavaVM **vm) {
    *vm = &g_vm;
    return 0;
}
jobject NewDirectByteBuffer(ENV, void *addr, jlong cap) {
    auto *b = make<Buffer>("java/nio/ByteBuffer");
    b->addr = addr;
    b->cap = cap;
    return (jobject)b;
}
void *GetDirectBufferAddress(ENV, jobject b) { return b ? ((Buffer *)b)->addr : nullptr; }
jlong GetDirectBufferCapacity(ENV, jobject b) { return b ? ((Buffer *)b)->cap : -1; }
jweak NewWeakGlobalRef(ENV, jobject o) { return o; }
void DeleteWeakGlobalRef(ENV, jweak) {}
jobjectRefType GetObjectRefType(ENV, jobject) { return JNIGlobalRefType; }
#undef ENV

jint vm_DestroyJavaVM(JavaVM *) { return JNI_OK; }
jint vm_AttachCurrentThread(JavaVM *, JNIEnv **e, void *) {
    *e = &g_env;
    return JNI_OK;
}
jint vm_DetachCurrentThread(JavaVM *) { return JNI_OK; }
jint vm_GetEnv(JavaVM *, void **e, jint) {
    *e = &g_env;
    return JNI_OK;
}

// Unfilled table entries: report which one the game needed.
template <size_t I> void trap() { fatal("fakejvm: JNI function table entry %zu is not implemented", I); }
template <size_t... I> void fill_traps(void **slots, std::index_sequence<I...>) { ((slots[I] = (void *)&trap<I>), ...); }

void build_tables() {
    constexpr size_t n = sizeof(JNINativeInterface) / sizeof(void *);
    fill_traps((void **)&g_iface, std::make_index_sequence<n>{});
    JNINativeInterface &t = g_iface;
    t.reserved0 = t.reserved1 = t.reserved2 = t.reserved3 = nullptr;
#define S(name) t.name = name
    S(GetVersion); S(FindClass); S(GetSuperclass); S(IsAssignableFrom); S(Throw); S(ThrowNew);
    S(ExceptionOccurred); S(ExceptionDescribe); S(ExceptionClear); S(FatalError); S(PushLocalFrame);
    S(PopLocalFrame); S(NewGlobalRef); S(DeleteGlobalRef); S(DeleteLocalRef); S(IsSameObject); S(NewLocalRef);
    S(EnsureLocalCapacity); S(AllocObject); S(NewObject); S(NewObjectV); S(NewObjectA); S(GetObjectClass);
    S(IsInstanceOf); S(GetMethodID); S(GetStaticMethodID); S(GetFieldID); S(GetStaticFieldID);
#define TYPED(Name)                                                                                          \
    S(Call##Name##Method); S(Call##Name##MethodV); S(Call##Name##MethodA); S(CallNonvirtual##Name##MethodA); \
    S(CallStatic##Name##Method); S(CallStatic##Name##MethodV); S(CallStatic##Name##MethodA);                 \
    S(Get##Name##Field); S(Set##Name##Field); S(GetStatic##Name##Field); S(SetStatic##Name##Field);
    TYPED(Object) TYPED(Boolean) TYPED(Byte) TYPED(Char) TYPED(Short) TYPED(Int) TYPED(Long) TYPED(Float) TYPED(Double)
#undef TYPED
    S(CallVoidMethod); S(CallVoidMethodV); S(CallVoidMethodA); S(CallNonvirtualVoidMethodA);
    S(CallStaticVoidMethod); S(CallStaticVoidMethodV); S(CallStaticVoidMethodA);
    S(NewString); S(GetStringLength); S(GetStringChars); S(ReleaseStringChars); S(NewStringUTF);
    S(GetStringUTFLength); S(GetStringUTFChars); S(ReleaseStringUTFChars); S(GetStringRegion); S(GetStringUTFRegion);
    S(GetArrayLength); S(NewObjectArray); S(GetObjectArrayElement); S(SetObjectArrayElement);
#define ARR(Name)                                                                                             \
    S(New##Name##Array); S(Get##Name##ArrayElements); S(Release##Name##ArrayElements);                       \
    S(Get##Name##ArrayRegion); S(Set##Name##ArrayRegion);
    ARR(Boolean) ARR(Byte) ARR(Char) ARR(Short) ARR(Int) ARR(Long) ARR(Float) ARR(Double)
#undef ARR
    S(GetPrimitiveArrayCritical); S(ReleasePrimitiveArrayCritical); S(RegisterNatives); S(UnregisterNatives);
    S(MonitorEnter); S(MonitorExit); S(GetJavaVM); S(NewDirectByteBuffer); S(GetDirectBufferAddress);
    S(GetDirectBufferCapacity); S(NewWeakGlobalRef); S(DeleteWeakGlobalRef); S(GetObjectRefType);
    t.ExceptionCheck = ExceptionCheck;
#undef S
    g_env.functions = &g_iface;
    g_invoke = JNIInvokeInterface{nullptr, nullptr, nullptr, vm_DestroyJavaVM, vm_AttachCurrentThread,
                                  vm_DetachCurrentThread, vm_GetEnv, vm_AttachCurrentThread};
    g_vm.functions = &g_invoke;
}
}  // namespace

void init(void *hwnd, int width, int height) {
    g_hwnd = hwnd;
    g_width = width;
    g_height = height;
    build_tables();
    define_classes();
    read_zip_dir(win::apk_path());
    g_activity = make<Obj>("com/doublefine/cq/CqActivity");
    g_assets = make<Obj>("android/content/res/AssetManager");
    g_surface = make<Obj>("android/view/Surface");
}

JavaVM *vm() { return &g_vm; }
JNIEnv *env() { return &g_env; }
jobject activity() { return (jobject)g_activity; }
jobject asset_manager() { return (jobject)g_assets; }
jobject surface() { return (jobject)g_surface; }
jstring new_string(const char *utf8) { return (jstring)str(utf8); }
void set_window_size(int width, int height) {
    g_width = width;
    g_height = height;
}

void *native(const char *c, const char *name) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    auto &n = cls(c)->natives;
    auto it = n.find(name);
    return it == n.end() ? nullptr : it->second;
}
jclass find_class(const char *name) { return (jclass)cls(name); }
}  // namespace fakejvm

// ---- the game window as an ANativeWindow
extern "C" {
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *, jobject) { return (ANativeWindow *)fakejvm::g_hwnd; }
void ANativeWindow_acquire(ANativeWindow *) {}
void ANativeWindow_release(ANativeWindow *) {}
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *, int32_t, int32_t, int32_t) { return 0; }
int32_t ANativeWindow_getWidth(ANativeWindow *) { return fakejvm::g_width; }
int32_t ANativeWindow_getHeight(ANativeWindow *) { return fakejvm::g_height; }
}
