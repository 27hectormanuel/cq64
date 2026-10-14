// SPDX-License-Identifier: CC0-1.0
// Keeps the game's busiest thread on the fastest CPU core.
//
// Android places threads by their recent load, and the emulated game's threads rarely look busy
// enough to it: on a Pixel 8 Pro the game thread, which bounds the frame rate, ran on the middle
// cores and its workers mostly on the small ones, while the fast core sat idle. A monitor thread
// looks at the game's threads (those the guest creates are all named "SDLThread") every two
// seconds: the busiest goes to the fastest core(s), the others to the next tier, never the
// slowest. Cores are grouped by their maximum frequency, so any big.LITTLE layout works.
//
// Placement alone is not enough: the governor clocks a core by how busy its thread looks, and
// the game's threads hand frames to each other, so each looks half idle and the clocks stay low
// (the fast core ran at 1.3 of 2.9 GHz). Android's Performance Hint API (ADPF, Android 12+) is
// meant for this: the game's threads join a hint session with a 60 fps frame-time target, each
// presented frame reports how long it really took, and Android raises the clocks to meet it.
#include "affinity.h"

#include "common.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace affinity {
namespace {
// ---- Performance Hint API (libandroid, API 33; loaded at run time)
struct Adpf {
    void *(*get_manager)();
    void *(*create)(void *, const int32_t *, size_t, int64_t);
    int (*report)(void *, int64_t);
    int (*set_threads)(void *, const int32_t *, size_t);  // API 34, may be null
    void (*close)(void *);
} g_adpf;
std::mutex g_session_lock;
void *g_session;
constexpr int64_t TARGET_NS = 1000000000LL / 60;

bool load_adpf() {
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return false;
    g_adpf.get_manager = (void *(*)())dlsym(lib, "APerformanceHint_getManager");
    g_adpf.create = (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
    g_adpf.report = (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    g_adpf.set_threads = (int (*)(void *, const int32_t *, size_t))dlsym(lib, "APerformanceHint_setThreads");
    g_adpf.close = (void (*)(void *))dlsym(lib, "APerformanceHint_closeSession");
    return g_adpf.get_manager && g_adpf.create && g_adpf.report && g_adpf.close;
}

// Puts `tids` in the hint session (creating it the first time).
void hint_threads(const std::vector<int32_t> &tids) {
    std::lock_guard<std::mutex> lk(g_session_lock);
    if (g_session && g_adpf.set_threads && g_adpf.set_threads(g_session, tids.data(), tids.size()) == 0) return;
    if (g_session) g_adpf.close(g_session), g_session = nullptr;
    void *manager = g_adpf.get_manager();
    if (manager) g_session = g_adpf.create(manager, tids.data(), tids.size(), TARGET_NS);
    LOGI("affinity: performance hint session %s for %zu threads", g_session ? "created" : "unavailable", tids.size());
}

// CPU masks by tier: the fastest cores, then the rest except the slowest tier.
bool core_tiers(cpu_set_t &fast, cpu_set_t &mid) {
    std::map<long, std::vector<int>, std::greater<long>> by_freq;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(path, "r");
        if (!f) break;
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1) by_freq[khz].push_back(c);
        fclose(f);
    }
    if (by_freq.size() < 2) return false;  // all cores alike: nothing to do
    CPU_ZERO(&fast);
    CPU_ZERO(&mid);
    auto tier = by_freq.begin();
    for (int c : tier->second) CPU_SET(c, &fast);
    // The middle tiers; with only two tiers, the fast cores themselves.
    if (by_freq.size() == 2) {
        for (int c : tier->second) CPU_SET(c, &mid);
    } else {
        for (auto t = std::next(tier); t != std::prev(by_freq.end()); ++t)
            for (int c : t->second) CPU_SET(c, &mid);
    }
    return true;
}

long thread_cpu_ticks(const char *tid, std::string &name) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/self/task/%s/stat", tid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *open = strchr(buf, '('), *close = strrchr(buf, ')');
    if (!open || !close) return -1;
    name.assign(open + 1, close - open - 1);
    // Fields after the name: state is field 3; utime and stime are 14 and 15.
    long utime = 0, stime = 0;
    if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &utime, &stime) != 2) return -1;
    return utime + stime;
}

void monitor() {
    pthread_setname_np(pthread_self(), "cq-affinity");
    if (!load_adpf()) g_adpf = {};
    cpu_set_t fast, mid;
    if (!core_tiers(fast, mid)) return;
    std::map<pid_t, long> last;
    pid_t pinned = 0;
    for (;;) {
        sleep(2);
        DIR *d = opendir("/proc/self/task");
        if (!d) return;
        std::vector<std::pair<long, pid_t>> busy;  // ticks used since the last look
        std::map<pid_t, long> now;
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            std::string name;
            long ticks = thread_cpu_ticks(e->d_name, name);
            if (ticks < 0 || name != "SDLThread") continue;
            pid_t tid = atoi(e->d_name);
            now[tid] = ticks;
            busy.push_back({ticks - (last.count(tid) ? last[tid] : ticks), tid});
        }
        closedir(d);
        last.swap(now);
        if (busy.empty()) continue;
        std::sort(busy.rbegin(), busy.rend());
        pid_t top = busy[0].first > 0 ? busy[0].second : pinned;
        for (const auto &[ticks, tid] : busy) sched_setaffinity(tid, sizeof(cpu_set_t), tid == top ? &fast : &mid);
        if (top != pinned) LOGI("affinity: game thread %d on the fastest cores", top);
        pinned = top;
        // The hint session: the game's threads that did real work in the last two seconds.
        static std::vector<int32_t> hinted;
        std::vector<int32_t> tids;
        for (const auto &[ticks, tid] : busy)
            if (ticks > 10 && tids.size() < 8) tids.push_back(tid);  // > 0.1 s of CPU
        std::sort(tids.begin(), tids.end());
        if (g_adpf.create && !tids.empty() && tids != hinted) hint_threads(tids), hinted = tids;
    }
}
}  // namespace

void frame_presented() {
    static int64_t last = 0;
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t now = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
    int64_t dt = last ? now - last : 0;
    last = now;
    if (dt <= 0 || dt > 500000000LL) return;  // first frame, or after a pause
    std::lock_guard<std::mutex> lk(g_session_lock);
    if (g_session) g_adpf.report(g_session, dt);
}

void start(const std::string &flag_file) {
    if (access(flag_file.c_str(), F_OK) != 0) return;
    std::thread(monitor).detach();
}
}  // namespace affinity
