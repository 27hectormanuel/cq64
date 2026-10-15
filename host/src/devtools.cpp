// SPDX-License-Identifier: CC0-1.0
// Developer tools.
//
// Debug command line: the retail libCq compiled out console typing (Console::HandleInput is
// `return 0`) and the debug menu, but Console::ExecuteCommand still runs Lua in the game's
// console environment and prints the result on screen. Commands are queued here and executed
// on the game thread, from the per-frame FMOD::EventSystem::update call, since Lua is not
// thread-safe.
//
// The console's own on-screen text drawing is stripped too, so each command runs inside a Lua
// wrapper that sends print() output, return values and errors to a host function (hostprint).
// Output goes to logcat, files/console.out (for adb) and a buffer the Java dev menu shows.
//
// Also tracks the presented frame rate and paces frames to a cap chosen from Java (manual, or
// from Android Game Mode).
#include "devtools.h"

#include "abi.h"
#include "game.h"
#include "affinity.h"
#include "luahints.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <time.h>
#include <unistd.h>

namespace devtools {
namespace {
std::mutex g_lock;
std::deque<std::string> g_queue;
std::atomic<float> g_fps{0};
std::atomic<int> g_cap_fps{0};  // 0 = uncapped

std::mutex g_out_lock;
std::string g_out;   // recent console output, oldest first
std::string g_list;  // text from hostlist() (the console's command list), not shown as output

void output(const std::string &line) {
    LOGI("console: %s", line.c_str());
    {
        std::lock_guard<std::mutex> lk(g_out_lock);
        g_out += line;
        g_out += '\n';
        if (g_out.size() > 16384) g_out.erase(0, g_out.size() - 16384);
    }
    if (FILE *f = fopen((game::files_dir() + "/console.out").c_str(), "a")) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }
}

u32 sym(const char *module, const char *name) {
    Module *m = loader::find(module);
    u32 a = m ? loader::lookup_in(m, name) : 0;
    if (!a) fatal("%s lacks %s", module, name);
    return a;
}

// The game library (last in the profile's list), which hosts the engine's console and Lua.
const char *game_lib() { return game::kLibs.back().soname; }

// lua_CFunction: hostprint(text)
s32 t_hostprint(u32 L) {
    static const u32 tolstring = sym(game_lib(), "lua_tolstring");
    u32 len = mem::halloc(4);
    u32 p = cpu::call32(tolstring, {L, 1, len});
    output(p ? std::string((const char *)g2h(p), rd<u32>(len)) : "(nil)");
    mem::hfree(len);
    return 0;
}

// lua_CFunction: hostlist(text) -- appends to the command list the dev menu reads
s32 t_hostlist(u32 L) {
    static const u32 tolstring = sym(game_lib(), "lua_tolstring");
    u32 len = mem::halloc(4);
    u32 p = cpu::call32(tolstring, {L, 1, len});
    if (p) {
        std::lock_guard<std::mutex> lk(g_out_lock);
        g_list.append((const char *)g2h(p), rd<u32>(len));
    }
    mem::hfree(len);
    return 0;
}

// Wraps a command so its output reaches hostprint. Expressions are tried as "return <cmd>"
// first, like an interactive Lua prompt.
std::string wrap(const std::string &cmd) {
    std::string eq;
    while (cmd.find("]" + eq + "]") != std::string::npos) eq += '=';
    std::string q = "[" + eq + "[" + cmd + "]" + eq + "]";
    return "local c = " + q + "\n"
           "local f, e = loadstring('return ' .. c, '=console')\n"
           "if not f then f, e = loadstring(c, '=console') end\n"
           "if not f then hostprint('error: ' .. tostring(e)) return end\n"
           "local p = print\n"
           "print = function(...) local t = {} for i = 1, select('#', ...) do "
           "t[#t + 1] = tostring((select(i, ...))) end hostprint(table.concat(t, '\\t')) end\n"
           "local r = {pcall(f)}\n"
           "print = p\n"
           "if not r[1] then hostprint('error: ' .. tostring(r[2])) return end\n"
           "for i = 2, table.maxn(r) do hostprint(tostring(r[i])) end\n";
}

void run_queued() {
    std::deque<std::string> cmds;
    // Only Double Fine's own engine (Costume Quest) has this console.
    static const u32 console_ptr = [] {
        Module *m = loader::find(game_lib());
        return m ? loader::lookup_in(m, "g_pConsole") : 0u;
    }();
    if (!console_ptr) return;
    u32 console = rd<u32>(console_ptr);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_queue.empty() || !console) return;  // commands wait for the console to exist
        cmds.swap(g_queue);
    }
    static const u32 copy = sym(game_lib(), "_ZN6String4CopyEPKci");
    static const u32 exec = sym(game_lib(), "_ZN7Console14ExecuteCommandERK6Stringi");
    static bool registered = false;
    if (!registered) {  // _G.hostprint, _G.hostlist = <host thunks>
        u32 L = rd<u32>(console + 0x18);
        auto set_global = [&](const char *name, u32 fn) {
            u32 n = mem::hstrdup(name);
            cpu::call32(sym(game_lib(), "lua_pushcclosure"), {L, fn, 0});
            cpu::call32(sym(game_lib(), "lua_setfield"), {L, (u32)-10002, n});  // LUA_GLOBALSINDEX
            mem::hfree(n);
        };
        set_global("hostprint", cpu::add_thunk("hostprint", &abi::wrapped<t_hostprint>));
        set_global("hostlist", cpu::add_thunk("hostlist", &abi::wrapped<t_hostlist>));
        registered = true;
    }
    for (const std::string &c : cmds) {
        // Commands starting with \x01 are the dev menu's own Lua: run as is, not echoed.
        bool raw = !c.empty() && c[0] == '\x01';
        if (!raw) output("> " + c);
        u32 text = mem::hstrdup(raw ? c.c_str() + 1 : wrap(c).c_str());
        u32 str = mem::halloc(16);  // String (Array<char>): count|flags, capacity|flags, data
        memset(g2h(str), 0, 16);
        cpu::call32(copy, {str, text, 0xffffffff});
        cpu::call32(exec, {console, str, 0xffffffff});  // player index -1: run locally
        mem::hfree(text);
        // The String's buffer belongs to the game's allocator; a few bytes per command leak.
        mem::hfree(str);
    }
}

s32 t_EventSystem_update(u32 self) {
    run_queued();
    return (s32)cpu::call32(sym("libfmodevent.so", "_ZN4FMOD11EventSystem6updateEv"), {self});
}
THUNK(override:_ZN4FMOD11EventSystem6updateEv, t_EventSystem_update);

std::string jstr(JNIEnv *env, jstring s) {
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}
void j_command(JNIEnv *env, jclass, jstring cmd) { submit(jstr(env, cmd)); }
jstring j_output(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lk(g_out_lock);
    return env->NewStringUTF(g_out.c_str());
}
jstring j_command_list(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lk(g_out_lock);
    return env->NewStringUTF(g_list.c_str());
}
// Built on first call (scans the game's code and scripts): call it off the UI thread.
jstring j_command_hints(JNIEnv *env, jclass) { return env->NewStringUTF(luahints::get().c_str()); }
jfloat j_fps(JNIEnv *, jclass) { return g_fps; }
void j_set_frame_cap(JNIEnv *, jclass, jint fps) {
    LOGI("frame cap -> %d", fps);
    g_cap_fps = fps < 0 ? 0 : fps;
}

s64 ns(const timespec &t) { return (s64)t.tv_sec * 1000000000 + t.tv_nsec; }
}  // namespace

void submit(const std::string &cmd) {
    if (cmd.empty()) return;
    std::lock_guard<std::mutex> lk(g_lock);
    g_queue.push_back(cmd);
}

std::string output_text() {
    std::lock_guard<std::mutex> lk(g_out_lock);
    return g_out;
}

std::string command_list() {
    std::lock_guard<std::mutex> lk(g_out_lock);
    return g_list;
}

void clear_output() {
    std::lock_guard<std::mutex> lk(g_out_lock);
    g_out.clear();
}

void frame_presented() {
    affinity::frame_presented();
    // Pace to the cap: sleep until this frame's slot, resetting the schedule after a stall.
    static s64 next = 0;
    if (int cap = g_cap_fps) {
        s64 period = 1000000000LL / cap;
        timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        s64 now = ns(t);
        if (next - now > 0 && next - now <= period) {
            timespec d{(time_t)((next - now) / 1000000000), (long)((next - now) % 1000000000)};
            nanosleep(&d, nullptr);
        } else {
            next = now;
        }
        next += period;
    } else {
        next = 0;
    }

    static int frames = 0;
    static timespec last{};
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    frames++;
    double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
    if (dt >= 0.5) {
        if (last.tv_sec) g_fps = (float)(frames / dt);
        frames = 0;
        last = now;
    }
}

void register_natives(JNIEnv *env) {
    unlink((game::files_dir() + "/console.out").c_str());  // one session's output at a time
    std::string cls = game::kPackage;
    for (char &ch : cls) if (ch == '.') ch = '/';
    jclass c = env->FindClass((cls + "/DevTools").c_str());
    if (!c) {
        env->ExceptionClear();
        return;
    }
    static const JNINativeMethod m[] = {
        {"nativeCommand", "(Ljava/lang/String;)V", (void *)&j_command},
        {"nativeOutput", "()Ljava/lang/String;", (void *)&j_output},
        {"nativeFps", "()F", (void *)&j_fps},
        {"nativeSetFrameCap", "(I)V", (void *)&j_set_frame_cap},
        {"nativeCommandList", "()Ljava/lang/String;", (void *)&j_command_list},
        {"nativeCommandHints", "()Ljava/lang/String;", (void *)&j_command_hints},
    };
    if (env->RegisterNatives(c, m, 6) != JNI_OK) fatal("RegisterNatives DevTools failed");
    env->DeleteLocalRef(c);
}
}  // namespace devtools
