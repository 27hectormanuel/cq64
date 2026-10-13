// SPDX-License-Identifier: CC0-1.0
// Entry point: loaded by Java in place of libCq/libfmodex/libfmodevent. Boots the emulator,
// loads the ARM32 libraries and forwards the Java native methods into them.
#include "abi.h"
#include "datapatch.h"
#include "devtools.h"
#include "gamepad.h"
#include "jni_bridge.h"
#include "paths.h"

#include <dlfcn.h>
#include <jni.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <type_traits>
#include <vector>

namespace libc_host { void init(); }

namespace {
struct NativeDef {
    const char *cls, *name, *sig;
    void *fn;
    u32 guest = 0;
};
extern NativeDef g_natives[];

struct Packer {
    std::vector<u32> w;
    void word(u32 v) { w.push_back(v); }
    void dword(u64 v) {
        if (w.size() & 1) w.push_back(0);  // AAPCS: 64-bit values start at an even slot
        w.push_back((u32)v);
        w.push_back((u32)(v >> 32));
    }
};
void pack(Packer &p, jint v) { p.word((u32)v); }
void pack(Packer &p, jboolean v) { p.word(v); }
void pack(Packer &p, jfloat v) { u32 x; memcpy(&x, &v, 4); p.word(x); }
void pack(Packer &p, jlong v) { p.dword((u64)v); }
void pack(Packer &p, jdouble v) { u64 x; memcpy(&x, &v, 8); p.dword(x); }
void pack(Packer &p, jobject v) { p.word(jni::to_guest(v)); }

template <int I, class R, class... A> R tramp(JNIEnv *env, jobject self, A... a) {
    jni::NativeFrame frame;
    Packer p;
    p.word(jni::guest_env());
    p.word(jni::to_guest(self));
    (pack(p, a), ...);
    u64 r = cpu::call(g_natives[I].guest, p.w.data(), p.w.size());
    if constexpr (std::is_void_v<R>) {
        (void)r;
    } else if constexpr (std::is_same_v<R, jobject>) {
        return env->NewLocalRef(jni::to_host((u32)r));
    } else {
        return (R)(u32)r;
    }
}

using O = jobject;
#define N(i, cls, name, sig, ...) {cls, name, sig, (void *)&tramp<i, __VA_ARGS__>}
constexpr const char *SDL = "org/libsdl/app/SDLActivity";
constexpr const char *CQ = "com/doublefine/cq/CqActivity";
NativeDef g_natives[] = {
    N(0, SDL, "nativeFlipBuffers", "()V", void),
    N(1, SDL, "nativeInit", "()V", void),
    N(2, SDL, "nativeLowMemory", "()V", void),
    N(3, SDL, "nativePause", "()V", void),
    N(4, SDL, "nativePause2", "()V", void),
    N(5, SDL, "nativeQuit", "()V", void),
    N(6, SDL, "nativeResume", "()V", void),
    N(7, SDL, "nativeResume2", "()V", void),
    N(8, SDL, "nativeSetMaxScreenResolution", "(II)V", void, jint, jint),
    N(9, SDL, "nativeUpdateScaledWindowBufferSize", "(IILjava/lang/Object;)V", void, jint, jint, O),
    N(10, SDL, "onNativeAccel", "(FFF)V", void, jfloat, jfloat, jfloat),
    N(11, SDL, "onNativeKeyDown", "(I)V", void, jint),
    N(12, SDL, "onNativeKeyUp", "(I)V", void, jint),
    N(13, SDL, "onNativeKeyboardFocusLost", "()V", void),
    N(14, SDL, "onNativeResize", "(III)V", void, jint, jint, jint),
    N(15, SDL, "onNativeSurfaceChanged", "()V", void),
    N(16, SDL, "onNativeSurfaceDestroyed", "()V", void),
    N(17, SDL, "onNativeTouch", "(IIIFFF)V", void, jint, jint, jint, jfloat, jfloat, jfloat),
    N(18, "org/libsdl/app/SDLInputConnection", "nativeCommitText", "(Ljava/lang/String;I)V", void, O, jint),
    N(19, "org/libsdl/app/SDLInputConnection", "nativeSetComposingText", "(Ljava/lang/String;I)V", void, O, jint),
    N(20, "org/fmod/FMODAudioDevice", "fmodGetInfo", "(I)I", jint, jint),
    N(21, "org/fmod/FMODAudioDevice", "fmodProcess", "(Ljava/nio/ByteBuffer;)I", jint, O),
    N(22, CQ, "dfNativeInit",
      "(Landroid/app/Activity;Landroid/content/res/AssetManager;Ljava/lang/String;Ljava/lang/String;)V", void, O, O,
      O, O),
    N(23, CQ, "dfNativeSetLanguage", "(Ljava/lang/String;)V", void, O),
    N(24, CQ, "dfNativeShutdown", "()V", void),
    N(25, CQ, "dfTrialPurchaseComplete", "(Z)V", void, jboolean),
    N(26, CQ, "setDeviceInfo",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V",
      void, O, O, O, O, O, O, jboolean),
};
#undef N

std::string jni_mangle(const char *cls, const char *name) {
    std::string s = "Java_";
    for (const char *p = cls; *p; p++) s += (*p == '/') ? '_' : *p;
    s += '_';
    s += name;
    return s;
}

std::vector<u8> read_file(const std::string &path) {
    std::vector<u8> v;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) fatal("cannot open %s", path.c_str());
    fseek(f, 0, SEEK_END);
    v.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(v.data(), 1, v.size(), f) != v.size()) fatal("short read on %s", path.c_str());
    fclose(f);
    return v;
}

void mkdirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0700);
}

// The installed APK path, as mapped by the runtime.
std::string own_apk() {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return {};
    char line[1024];
    std::string apk;
    while (fgets(line, sizeof line, f)) {
        const char *p = strchr(line, '/');
        if (!p) continue;
        std::string path(p);
        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
        if (path.find("/com.doublefine.cq-") != std::string::npos && path.size() > 9 &&
            path.compare(path.size() - 9, 9, "/base.apk") == 0) {
            apk = path;
            break;
        }
    }
    fclose(f);
    return apk;
}

std::string own_dir() {
    Dl_info info{};
    dladdr((void *)&own_dir, &info);
    std::string p = info.dli_fname ? info.dli_fname : "";
    return p.substr(0, p.rfind('/'));
}
}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    LOGI("cqhost starting");
    mem::init();
    cpu::init();
    jni::init(vm);

    // The game keeps config and saves under /sdcard/dfp, which modern Android denies to apps
    // without storage permission; redirect the old shared-storage roots into private storage.
    const std::string storage = "/data/data/com.doublefine.cq/files/sdcard";
    mkdirs(storage + "/dfp/cq-droid/_write");
    for (const char *root : {"/sdcard/", "/mnt/sdcard/", "/storage/emulated/0/"})
        paths::add_mapping(root, storage + "/");

    // FMOD checks /proc/cpuinfo for the ARMv7 "vfpv3"/"neon" features and refuses to set up any
    // output without them; a 64-bit process sees arm64 names ("fp asimd"), so give the guest the
    // view a 32-bit process would have.
    const std::string cpuinfo = "/data/data/com.doublefine.cq/files/cpuinfo32";
    if (FILE *f = fopen(cpuinfo.c_str(), "w")) {
        for (int i = 0; i < 8; i++)
            fprintf(f,
                    "processor\t: %d\nmodel name\t: ARMv7 Processor rev 0 (v7l)\nBogoMIPS\t: 38.40\n"
                    "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt lpae evtstrm\n"
                    "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xd05\n"
                    "CPU revision\t: 0\n\n",
                    i);
        fputs("Hardware\t: Google Tensor\n", f);
        fclose(f);
        paths::add_mapping("/proc/cpuinfo", cpuinfo);
    }

    // The game reads its 1 GB data pack (assets/ant.mp3) out of its own APK. A slim APK without it
    // is used when the phone lacks room for the full one; the pack then lives in a zip streamed
    // into private storage (see install_split.sh), and reads of the APK are redirected there.
    const std::string data_zip = "/data/data/com.doublefine.cq/files/cqdata.zip";
    if (access(data_zip.c_str(), R_OK) == 0) {
        std::string apk = own_apk();
        if (!apk.empty()) paths::add_mapping(apk, data_zip);
        else LOGW("data zip present but own APK path not found");
        datapatch::init(data_zip);
    } else {
        datapatch::init(own_apk());
    }

    // The ARM32 libraries ship next to us, renamed so the installer treats them as plain files.
    std::string dir = own_dir();
    struct { const char *file, *soname; } libs[] = {
        {"libguest_rt.so", "libguestrt.so"},
        {"libguest_fmodex.so", "libfmodex.so"},
        {"libguest_fmodevent.so", "libfmodevent.so"},
        {"libguest_Cq.so", "libCq.so"},
    };
    std::vector<Module *> mods;
    for (auto &l : libs) {
        mods.push_back(loader::load(l.soname, read_file(dir + "/" + l.file)));
        if (mods.size() == 1) libc_host::init();  // needs the guest runtime's __sF
    }
    for (Module *m : mods) loader::run_init(m);

    Module *cq = loader::find("libCq.so");
    Module *fmod = loader::find("libfmodex.so");
    gamepad::install(cq);
    if (u32 onload = loader::lookup_in(cq, "JNI_OnLoad")) {
        u32 v = cpu::call32(onload, {jni::guest_vm(), 0});
        LOGI("guest JNI_OnLoad returned 0x%x", v);
    }

    JNIEnv *env = jni::host_env();
    for (auto &n : g_natives) {
        std::string sym = jni_mangle(n.cls, n.name);
        n.guest = loader::lookup_in(cq, sym.c_str());
        if (!n.guest) n.guest = loader::lookup_in(fmod, sym.c_str());
        if (!n.guest) fatal("guest does not export %s", sym.c_str());
        jclass c = env->FindClass(n.cls);
        if (!c) fatal("class %s not found", n.cls);
        JNINativeMethod m{n.name, n.sig, n.fn};
        if (env->RegisterNatives(c, &m, 1) != JNI_OK) fatal("RegisterNatives %s.%s failed", n.cls, n.name);
        env->DeleteLocalRef(c);
    }
    devtools::register_natives(env);
    gamepad::register_natives(env);
    // Debug aids, polled once a second:
    //   adb shell run-as com.doublefine.cq touch files/dump           dumps all guest threads
    //   echo 'cmd' | adb shell run-as com.doublefine.cq sh -c 'cat > files/console'
    //                                                                  runs console commands
    //   echo '1 150' | ... sh -c 'cat > files/pad'   presses virtual pad buttons (mask, ms) in turn
    std::thread([] {
        const char *dump = "/data/data/com.doublefine.cq/files/dump";
        const char *console = "/data/data/com.doublefine.cq/files/console";
        for (;;) {
            if (FILE *f = fopen("/data/data/com.doublefine.cq/files/pad", "r")) {
                unsigned mask;
                int ms;
                while (fscanf(f, "%i %d", &mask, &ms) == 2) {
                    gamepad::debug_press(mask, ms);
                    usleep(300 * 1000);
                }
                fclose(f);
                unlink("/data/data/com.doublefine.cq/files/pad");
            }
            if (access(dump, F_OK) == 0) {
                unlink(dump);
                cpu::request_dump_all();
            }
            if (FILE *f = fopen(console, "r")) {
                char line[4096];
                while (fgets(line, sizeof line, f)) {
                    std::string cmd(line);
                    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
                    devtools::submit(cmd);
                }
                fclose(f);
                unlink(console);
            }
            sleep(1);
        }
    }).detach();
    LOGI("cqhost ready");
    return JNI_VERSION_1_6;
}
