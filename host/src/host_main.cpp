// SPDX-License-Identifier: CC0-1.0
// Entry point: loaded by Java in place of the game's 32-bit libraries. Boots the emulator, loads
// the ARM32 libraries listed by the game profile (game_<name>.cpp) and forwards the Java native
// methods into them.
#include "abi.h"
#include "game.h"
#include "jni_bridge.h"
#include "natives.h"
#include "paths.h"

#include <dlfcn.h>
#include <jni.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

namespace libc_host { void init(); }

namespace game {
std::string files_dir() { return std::string("/data/data/") + kPackage + "/files"; }
}  // namespace game

namespace host {
void mkdirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0700);
}

std::string own_apk() {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return {};
    const std::string marker = std::string("/") + game::kPackage + "-";
    char line[1024];
    std::string apk;
    while (fgets(line, sizeof line, f)) {
        const char *p = strchr(line, '/');
        if (!p) continue;
        std::string path(p);
        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
        if (path.find(marker) != std::string::npos && path.size() > 9 &&
            path.compare(path.size() - 9, 9, "/base.apk") == 0) {
            apk = path;
            break;
        }
    }
    fclose(f);
    return apk;
}
}  // namespace host

namespace {
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

std::string own_dir() {
    Dl_info info{};
    dladdr((void *)&own_dir, &info);
    std::string p = info.dli_fname ? info.dli_fname : "";
    return p.substr(0, p.rfind('/'));
}
}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    LOGI("cqhost starting (%s)", game::kPackage);
    mem::init();
    cpu::init();
    jni::init(vm);
    host::mkdirs(game::files_dir());

    // FMOD checks /proc/cpuinfo for the ARMv7 "vfpv3"/"neon" features and refuses to set up any
    // output without them; a 64-bit process sees arm64 names ("fp asimd"), so give the guest the
    // view a 32-bit process would have.
    const std::string cpuinfo = game::files_dir() + "/cpuinfo32";
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

    // Guest crash reports also go to files/crash.log: logcat can drop them when the game logs a lot.
    setenv("CQ_CRASH_LOG", (game::files_dir() + "/crash.log").c_str(), 1);
    game::setup();

    // The ARM32 libraries ship next to us, renamed so the installer treats them as plain files.
    std::string dir = own_dir();
    std::vector<Module *> mods;
    for (auto &l : game::kLibs) {
        mods.push_back(loader::load(l.soname, read_file(dir + "/" + l.file)));
        if (mods.size() == 1) libc_host::init();  // needs the guest runtime's __sF
    }
    for (Module *m : mods) loader::run_init(m);

    Module *main = mods.back();
    game::after_load(main);
    if (u32 onload = loader::lookup_in(main, "JNI_OnLoad")) {
        u32 v = cpu::call32(onload, {jni::guest_vm(), 0});
        LOGI("guest JNI_OnLoad returned 0x%x", v);
    }

    JNIEnv *env = jni::host_env();
    for (size_t i = 0; i < g_native_count; i++) {
        NativeDef &n = g_natives[i];
        std::string sym = jni_mangle(n.cls, n.name);
        for (auto it = mods.rbegin(); it != mods.rend() && !n.guest; ++it) n.guest = loader::lookup_in(*it, sym.c_str());
        if (!n.guest) fatal("guest does not export %s", sym.c_str());
        jclass c = env->FindClass(n.cls);
        if (!c) fatal("class %s not found", n.cls);
        JNINativeMethod m{n.name, n.sig, n.fn};
        if (env->RegisterNatives(c, &m, 1) != JNI_OK) fatal("RegisterNatives %s.%s failed", n.cls, n.name);
        env->DeleteLocalRef(c);
    }
    game::register_natives(env);

    // Debug aids, polled once a second:
    //   adb shell run-as <package> touch files/dump      dumps all guest threads
    // plus the game profile's own triggers (game::poll_debug).
    std::thread([] {
        const std::string dump = game::files_dir() + "/dump";
        for (;;) {
            if (access(dump.c_str(), F_OK) == 0) {
                unlink(dump.c_str());
                cpu::request_dump_all();
            }
            game::poll_debug();
            sleep(1);
        }
    }).detach();
    LOGI("cqhost ready");
    return JNI_VERSION_1_6;
}
