// SPDX-License-Identifier: CC0-1.0
// Host implementations of libc imports that need the real OS (files, time, math, process).
// Guest structures follow the 32-bit bionic ABI and are converted field by field.
#include "abi.h"
#include "datapatch.h"
#include "paths.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <math.h>
#include <poll.h>
#include <wchar.h>
#include <wctype.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace {
GuestThread &T() { return cur_thread(); }
int fail(int e = errno) {
    T().set_errno(e);
    return -1;
}
template <class R> R check(R r) {
    if (r == (R)-1) T().set_errno(errno);
    return r;
}

// ---------------------------------------------------------------- stdio
constexpr u32 GFILE_SIZE = 84;
constexpr u16 SEOF = 0x0020, SERR = 0x0040;
std::mutex g_files_lock;
std::unordered_map<u32, FILE *> g_files;
u32 g_sF = 0;  // guest __sF array (stdin, stdout, stderr)

struct LogStream {
    std::mutex lock;
    std::string buf;
    int prio;
    void write(const char *p, size_t n) {
        std::lock_guard<std::mutex> lk(lock);
        for (size_t i = 0; i < n; i++) {
            if (p[i] == '\n') flush_locked();
            else buf += p[i];
        }
        if (buf.size() > 900) flush_locked();
    }
    void flush_locked() {
        if (!buf.empty()) __android_log_write(prio, "cq-guest", buf.c_str());
        buf.clear();
    }
};
LogStream g_out{.prio = ANDROID_LOG_INFO}, g_err{.prio = ANDROID_LOG_WARN};

int std_index(u32 gf) {
    if (g_sF && gf >= g_sF && gf < g_sF + 3 * GFILE_SIZE) return (gf - g_sF) / GFILE_SIZE;
    return -1;
}
FILE *hfile(u32 gf) {
    int si = std_index(gf);
    if (si == 0) return stdin;
    std::lock_guard<std::mutex> lk(g_files_lock);
    auto it = g_files.find(gf);
    return it == g_files.end() ? nullptr : it->second;
}
void sync_flags(u32 gf, FILE *f) {
    u16 fl = rd<u16>(gf + 12) & ~(SEOF | SERR);
    if (feof(f)) fl |= SEOF;
    if (ferror(f)) fl |= SERR;
    wr<u16>(gf + 12, fl);
}

// Wraps a host FILE in a guest FILE (only _file, at offset 14, is read by guest code).
u32 guest_file(FILE *f) {
    if (!f) return fail(), 0;
    u32 gf = mem::halloc(GFILE_SIZE);
    wr<s16>(gf + 14, (s16)fileno(f));
    std::lock_guard<std::mutex> lk(g_files_lock);
    g_files[gf] = f;
    return gf;
}

u32 t_fopen(G<const char> path, G<const char> mode) {
    std::string p = paths::translate(path);
    FILE *f = fopen(p.c_str(), mode);
    if (!f) {
        LOGI("fopen(%s, %s) failed: %s", p.c_str(), mode.get(), strerror(errno));
        return fail(), 0;
    }
    return guest_file(f);
}
THUNK(fopen, t_fopen);

int t_fclose(u32 gf) {
    if (std_index(gf) >= 0) return 0;
    FILE *f;
    {
        std::lock_guard<std::mutex> lk(g_files_lock);
        auto it = g_files.find(gf);
        if (it == g_files.end()) return fail(EBADF);
        f = it->second;
        g_files.erase(it);
    }
    mem::hfree(gf);
    return check(fclose(f));
}
THUNK(fclose, t_fclose);

u32 t_fread(G<void> buf, u32 size, u32 n, u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF), 0;
    off_t pos = ftello(f);
    size_t r = fread(buf, size, n, f);
    if (r) datapatch::apply(fileno(f), pos, buf, (s64)r * size);
    sync_flags(gf, f);
    return (u32)r;
}
THUNK(fread, t_fread);

u32 t_fwrite(G<const void> buf, u32 size, u32 n, u32 gf) {
    int si = std_index(gf);
    if (si == 1 || si == 2) {
        (si == 1 ? g_out : g_err).write((const char *)buf.get(), (size_t)size * n);
        return n;
    }
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF), 0;
    size_t r = fwrite(buf, size, n, f);
    sync_flags(gf, f);
    return (u32)r;
}
THUNK(fwrite, t_fwrite);

int t_fputs(G<const char> s, u32 gf) {
    size_t n = strlen(s);
    return t_fwrite(G<const void>{s.a}, 1, (u32)n, gf) == n ? 1 : -1;
}
THUNK(fputs, t_fputs);

int t_fputc(int c, u32 gf) {
    char ch = (char)c;
    int si = std_index(gf);
    if (si == 1 || si == 2) {
        (si == 1 ? g_out : g_err).write(&ch, 1);
        return (u8)c;
    }
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF);
    return fputc(c, f);
}
THUNK(fputc, t_fputc);

int t_fgetc(u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF);
    int c = fgetc(f);
    sync_flags(gf, f);
    return c;
}
THUNK(fgetc, t_fgetc);
THUNK(__srget, t_fgetc);
THUNK(getc, t_fgetc);

int t_ungetc(int c, u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return -1;
    int r = ungetc(c, f);
    sync_flags(gf, f);
    return r;
}
THUNK(ungetc, t_ungetc);

u32 t_fgets(G<char> buf, int n, u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return 0;
    char *r = fgets(buf, n, f);
    sync_flags(gf, f);
    return r ? buf.a : 0;
}
THUNK(fgets, t_fgets);

int t_fflush(u32 gf) {
    if (!gf) return 0;
    if (std_index(gf) >= 0) return 0;
    FILE *f = hfile(gf);
    return f ? fflush(f) : 0;
}
THUNK(fflush, t_fflush);

int t_fseek(u32 gf, s32 off, int whence) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF);
    int r = check(fseeko(f, off, whence));
    sync_flags(gf, f);
    return r;
}
THUNK(fseek, t_fseek);
THUNK(fseeko, t_fseek);

s32 t_ftell(u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF);
    return (s32)check(ftello(f));
}
THUNK(ftell, t_ftell);
THUNK(ftello, t_ftell);

void t_rewind(u32 gf) {
    if (FILE *f = hfile(gf)) {
        rewind(f);
        sync_flags(gf, f);
    }
}
THUNK(rewind, t_rewind);

int t_feof(u32 gf) { FILE *f = hfile(gf); return f ? feof(f) : 0; }
THUNK(feof, t_feof);
int t_ferror(u32 gf) { FILE *f = hfile(gf); return f ? ferror(f) : 0; }
THUNK(ferror, t_ferror);
int t_fileno(u32 gf) { FILE *f = hfile(gf); return f ? fileno(f) : fail(EBADF); }
THUNK(fileno, t_fileno);
int t_setvbuf(u32, u32, int, u32) { return 0; }
THUNK(setvbuf, t_setvbuf);

// ---------------------------------------------------------------- fd I/O
constexpr int A32_CREAT = 0100, A32_EXCL = 0200, A32_TRUNC = 01000, A32_APPEND = 02000, A32_NONBLOCK = 04000,
              A32_DIRECTORY = 040000, A32_NOFOLLOW = 0100000, A32_DIRECT = 0200000, A32_LARGEFILE = 0400000,
              A32_CLOEXEC = 02000000;

int xlate_open_flags(int g) {
    // ARM32 and arm64/x86_64 disagree on DIRECTORY..LARGEFILE; Windows (MinGW) also uses different
    // values for CREAT (0x100 vs 0100), EXCL (0x400 vs 0200) and APPEND (0x008 vs 02000).
    int h = g & ~(A32_CREAT | A32_EXCL | A32_TRUNC | A32_APPEND | A32_NONBLOCK | A32_DIRECTORY | A32_NOFOLLOW |
                  A32_DIRECT | A32_LARGEFILE | A32_CLOEXEC);
    if (g & A32_CREAT) h |= O_CREAT;
    if (g & A32_EXCL) h |= O_EXCL;
    if (g & A32_TRUNC) h |= O_TRUNC;
    if (g & A32_APPEND) h |= O_APPEND;
    if (g & A32_NONBLOCK) h |= O_NONBLOCK;
    if (g & A32_DIRECTORY) h |= O_DIRECTORY;
    if (g & A32_NOFOLLOW) h |= O_NOFOLLOW;
    if (g & A32_DIRECT) h |= O_DIRECT;
    if (g & A32_CLOEXEC) h |= O_CLOEXEC;
#ifdef O_BINARY
    h |= O_BINARY;
#endif
    return h;
}
int xlate_open_flags_back(int h) {
    int g = h & ~(O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_DIRECT | O_CLOEXEC);
#ifdef O_LARGEFILE
    g &= ~O_LARGEFILE;
#endif
#ifdef O_BINARY
    g &= ~O_BINARY;
#endif
    if (h & O_CREAT) g |= A32_CREAT;
    if (h & O_EXCL) g |= A32_EXCL;
    if (h & O_TRUNC) g |= A32_TRUNC;
    if (h & O_APPEND) g |= A32_APPEND;
    if (h & O_NONBLOCK) g |= A32_NONBLOCK;
    if (h & O_DIRECTORY) g |= A32_DIRECTORY;
    if (h & O_NOFOLLOW) g |= A32_NOFOLLOW;
    if (h & O_DIRECT) g |= A32_DIRECT;
    if (h & O_CLOEXEC) g |= A32_CLOEXEC;
    return g;
}

void t_open(GuestThread &t) {
    std::string p = paths::translate((const char *)g2h(t.r(0)));
    int flags = xlate_open_flags((int)t.r(1));
    mode_t mode = (mode_t)t.r(2);
#ifdef _WIN32
    mode |= 0666;  // _open uses mode for the read-only attribute; guest callers may omit it
#endif
    int r = open(p.c_str(), flags, mode);
    if (r < 0) {
        LOGI("open(%s) failed: %s", p.c_str(), strerror(errno));
        r = fail();
    }
    t.ret32((u32)r);
}
THUNK_RAW(open, t_open);

s32 t_read(int fd, G<void> buf, u32 n) {
    off_t pos = lseek(fd, 0, SEEK_CUR);
    ssize_t r = read(fd, buf, n);
    if (r > 0 && pos >= 0) datapatch::apply(fd, pos, buf, r);
    return (s32)check(r);
}
THUNK(read, t_read);
s32 t_write(int fd, G<const void> buf, u32 n) {
    if (fd == 1 || fd == 2) {
        (fd == 1 ? g_out : g_err).write((const char *)buf.get(), n);
        return (s32)n;
    }
    return (s32)check(write(fd, buf, n));
}
THUNK(write, t_write);
int t_close(int fd) { return check(close(fd)); }
THUNK(close, t_close);
s32 t_lseek(int fd, s32 off, int whence) {
    off_t r = lseek(fd, off, whence);
    if (r < 0) return fail();
    return (s32)r;
}
THUNK(lseek, t_lseek);
s64 t_lseek64(int fd, s64 off, int whence) {
    off_t r = lseek(fd, off, whence);
    if (r < 0) return fail();
    return r;
}
THUNK(lseek64, t_lseek64);
int t_fsync(int fd) { return check(fsync(fd)); }
THUNK(fsync, t_fsync);
int t_ftruncate(int fd, s32 len) { return check(ftruncate(fd, len)); }
THUNK(ftruncate, t_ftruncate);

void t_fcntl(GuestThread &t) {
    int fd = (int)t.r(0), cmd = (int)t.r(1);
    u32 arg = t.r(2);
    int r;
    switch (cmd) {
    case F_GETFL: r = fcntl(fd, F_GETFL); if (r >= 0) r = xlate_open_flags_back(r); break;
    case F_SETFL: r = fcntl(fd, F_SETFL, xlate_open_flags((int)arg)); break;
    case F_GETFD: case F_SETFD: case F_DUPFD: r = fcntl(fd, cmd, (int)arg); break;
    default:
        LOGW("fcntl: unsupported cmd %d", cmd);
        r = -1;
        errno = EINVAL;
    }
    t.ret32((u32)(r < 0 ? fail() : r));
}
THUNK_RAW(fcntl, t_fcntl);

void t_ioctl(GuestThread &t) {
    int fd = (int)t.r(0);
    u32 req = t.r(1), arg = t.r(2);
    int r;
    switch (req) {
    case FIONREAD: case FIONBIO: r = ioctl(fd, req, g2h(arg)); break;
    default:
        LOGW("ioctl: unsupported request 0x%x", req);
        r = -1;
        errno = ENOTTY;
    }
    t.ret32((u32)(r < 0 ? fail() : r));
}
THUNK_RAW(ioctl, t_ioctl);

// bionic LP32 struct stat (104 bytes)
void put_stat(u32 g, const struct stat &s) {
    memset(g2h(g), 0, 104);
    wr<u64>(g + 0, s.st_dev);
    wr<u32>(g + 12, (u32)s.st_ino);
    wr<u32>(g + 16, s.st_mode);
    wr<u32>(g + 20, (u32)s.st_nlink);
    wr<u32>(g + 24, s.st_uid);
    wr<u32>(g + 28, s.st_gid);
    wr<u64>(g + 32, s.st_rdev);
    wr<s64>(g + 48, s.st_size);
#ifdef _WIN32
    // Windows: whole-second times and no block fields.
    wr<u32>(g + 56, 4096);
    wr<u64>(g + 64, ((u64)s.st_size + 511) / 512);
    wr<u32>(g + 72, (u32)s.st_atime);
    wr<u32>(g + 80, (u32)s.st_mtime);
    wr<u32>(g + 88, (u32)s.st_ctime);
#else
    wr<u32>(g + 56, (u32)s.st_blksize);
    wr<u64>(g + 64, (u64)s.st_blocks);
    wr<u32>(g + 72, (u32)s.st_atim.tv_sec);
    wr<u32>(g + 76, (u32)s.st_atim.tv_nsec);
    wr<u32>(g + 80, (u32)s.st_mtim.tv_sec);
    wr<u32>(g + 84, (u32)s.st_mtim.tv_nsec);
    wr<u32>(g + 88, (u32)s.st_ctim.tv_sec);
    wr<u32>(g + 92, (u32)s.st_ctim.tv_nsec);
#endif
    wr<u64>(g + 96, s.st_ino);
}
int t_stat(G<const char> path, u32 out) {
    struct stat s;
    std::string p = paths::translate(path);
    if (stat(p.c_str(), &s) < 0) return fail();
    put_stat(out, s);
    return 0;
}
THUNK(stat, t_stat);
THUNK(lstat, t_stat);
int t_fstat(int fd, u32 out) {
    struct stat s;
    if (fstat(fd, &s) < 0) return fail();
    put_stat(out, s);
    return 0;
}
THUNK(fstat, t_fstat);

int t_access(G<const char> path, int mode) { return check(access(paths::translate(path).c_str(), mode)); }
THUNK(access, t_access);
int t_mkdir(G<const char> path, u32 mode) { return check(mkdir(paths::translate(path).c_str(), mode)); }
THUNK(mkdir, t_mkdir);
int t_rmdir(G<const char> path) { return check(rmdir(paths::translate(path).c_str())); }
THUNK(rmdir, t_rmdir);
int t_unlink(G<const char> path) { return check(unlink(paths::translate(path).c_str())); }
THUNK(unlink, t_unlink);
int t_remove(G<const char> path) { return check(remove(paths::translate(path).c_str())); }
THUNK(remove, t_remove);
int t_rename(G<const char> a, G<const char> b) {
    return check(rename(paths::translate(a).c_str(), paths::translate(b).c_str()));
}
THUNK(rename, t_rename);
int t_chmod(G<const char> path, u32 mode) { return check(chmod(paths::translate(path).c_str(), mode)); }
THUNK(chmod, t_chmod);
int t_chown(G<const char>, u32, u32) { return 0; }
THUNK(chown, t_chown);
int t_utime(G<const char> path, u32 times) {
    std::string p = paths::translate(path);
    if (!times) return check(utime(p.c_str(), nullptr));
    struct utimbuf u{(time_t)rd<s32>(times), (time_t)rd<s32>(times + 4)};
    return check(utime(p.c_str(), &u));
}
THUNK(utime, t_utime);
int t_utimes(G<const char> path, u32 tv) {
    std::string p = paths::translate(path);
    if (!tv) return check(utimes(p.c_str(), nullptr));
    struct timeval h[2];
    for (int i = 0; i < 2; i++) {
        h[i].tv_sec = rd<s32>(tv + i * 8);
        h[i].tv_usec = rd<s32>(tv + i * 8 + 4);
    }
    return check(utimes(p.c_str(), h));
}
THUNK(utimes, t_utimes);

// DIR*: guest handle is a guest struct dirent buffer (same layout on all bionic ABIs).
std::mutex g_dirs_lock;
std::unordered_map<u32, DIR *> g_dirs;
u32 t_opendir(G<const char> path) {
    std::string p = paths::translate(path);
    DIR *d = opendir(p.c_str());
    if (!d) return fail(), 0;
    u32 g = mem::halloc(sizeof(struct dirent));
    std::lock_guard<std::mutex> lk(g_dirs_lock);
    g_dirs[g] = d;
    return g;
}
THUNK(opendir, t_opendir);
u32 t_readdir(u32 g) {
    DIR *d;
    {
        std::lock_guard<std::mutex> lk(g_dirs_lock);
        auto it = g_dirs.find(g);
        if (it == g_dirs.end()) return 0;
        d = it->second;
    }
    struct dirent *e = readdir(d);
    if (!e) return 0;
    memcpy(g2h(g), e, sizeof(struct dirent));
    return g;
}
THUNK(readdir, t_readdir);
int t_closedir(u32 g) {
    DIR *d;
    {
        std::lock_guard<std::mutex> lk(g_dirs_lock);
        auto it = g_dirs.find(g);
        if (it == g_dirs.end()) return fail(EBADF);
        d = it->second;
        g_dirs.erase(it);
    }
    mem::hfree(g);
    return closedir(d);
}
THUNK(closedir, t_closedir);

int t_select(int n, G<fd_set> r, G<fd_set> w, G<fd_set> e, u32 tv) {
    struct timeval h, *hp = nullptr;
    if (tv) {
        h.tv_sec = rd<s32>(tv);
        h.tv_usec = rd<s32>(tv + 4);
        hp = &h;
    }
    int res = select(n, r, w, e, hp);
    if (tv) {
        wr<s32>(tv, (s32)h.tv_sec);
        wr<s32>(tv + 4, (s32)h.tv_usec);
    }
    return res < 0 ? fail() : res;
}
THUNK(select, t_select);

// ---------------------------------------------------------------- time
s32 t_time(u32 out) {
    s32 now = (s32)time(nullptr);
    if (out) wr<s32>(out, now);
    return now;
}
THUNK(time, t_time);
int t_gettimeofday(u32 tv, u32 tz) {
    struct timeval h;
    gettimeofday(&h, nullptr);
    if (tv) {
        wr<s32>(tv, (s32)h.tv_sec);
        wr<s32>(tv + 4, (s32)h.tv_usec);
    }
    if (tz) {
        wr<s32>(tz, 0);
        wr<s32>(tz + 4, 0);
    }
    return 0;
}
THUNK(gettimeofday, t_gettimeofday);
int t_clock_gettime(int clk, u32 ts) {
    struct timespec h;
    if (clock_gettime(clk, &h) < 0) return fail();
    wr<s32>(ts, (s32)h.tv_sec);
    wr<s32>(ts + 4, (s32)h.tv_nsec);
    return 0;
}
THUNK(clock_gettime, t_clock_gettime);
s32 t_clock() { return (s32)clock(); }
THUNK(clock, t_clock);
int t_nanosleep(u32 req, u32 rem) {
    struct timespec r{rd<s32>(req), rd<s32>(req + 4)}, left{};
    int res = nanosleep(&r, &left);
    if (rem) {
        wr<s32>(rem, (s32)left.tv_sec);
        wr<s32>(rem + 4, (s32)left.tv_nsec);
    }
    return res < 0 ? fail() : 0;
}
THUNK(nanosleep, t_nanosleep);
int t_usleep(u32 us) { return usleep(us); }
THUNK(usleep, t_usleep);
u32 t_sleep(u32 s) { return sleep(s); }
THUNK(sleep, t_sleep);
double t_difftime(s32 a, s32 b) { return (double)a - (double)b; }
THUNK(difftime, t_difftime);

// bionic LP32 struct tm: 9 ints, long tm_gmtoff, const char* tm_zone (44 bytes)
void put_tm(u32 g, const struct tm &t) {
    s32 v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    memcpy(g2h(g), v, sizeof v);
#ifndef _WIN32  // Windows' struct tm has no tm_gmtoff
    wr<s32>(g + 36, (s32)t.tm_gmtoff);
#endif
    static u32 zone_utc = mem::hstrdup("UTC");
    wr<u32>(g + 40, zone_utc);
}
struct tm get_tm(u32 g) {
    struct tm t{};
    s32 v[9];
    memcpy(v, g2h(g), sizeof v);
    t.tm_sec = v[0]; t.tm_min = v[1]; t.tm_hour = v[2]; t.tm_mday = v[3]; t.tm_mon = v[4];
    t.tm_year = v[5]; t.tm_wday = v[6]; t.tm_yday = v[7]; t.tm_isdst = v[8];
#ifndef _WIN32
    t.tm_gmtoff = rd<s32>(g + 36);
#endif
    return t;
}
u32 thread_tm() {
    thread_local u32 buf = mem::halloc(44);
    return buf;
}
u32 t_localtime(u32 tp) {
    time_t t = rd<s32>(tp);
    struct tm h;
    localtime_r(&t, &h);
    u32 g = thread_tm();
    put_tm(g, h);
    return g;
}
THUNK(localtime, t_localtime);
u32 t_gmtime(u32 tp) {
    time_t t = rd<s32>(tp);
    struct tm h;
    gmtime_r(&t, &h);
    u32 g = thread_tm();
    put_tm(g, h);
    return g;
}
THUNK(gmtime, t_gmtime);
u32 t_localtime_r(u32 tp, u32 out) {
    time_t t = rd<s32>(tp);
    struct tm h;
    localtime_r(&t, &h);
    put_tm(out, h);
    return out;
}
THUNK(localtime_r, t_localtime_r);
s32 t_mktime(u32 g) {
    struct tm h = get_tm(g);
    time_t r = mktime(&h);
    put_tm(g, h);
    return (s32)r;
}
THUNK(mktime, t_mktime);
u32 t_strftime(G<char> buf, u32 max, G<const char> fmt, u32 tmp) {
    struct tm h = get_tm(tmp);
    return (u32)strftime(buf, max, fmt, &h);
}
THUNK(strftime, t_strftime);

// ---------------------------------------------------------------- math
#define MATH1(fn) double t_##fn(double x) { return fn(x); } THUNK(fn, t_##fn)
#define MATH1F(fn) float t_##fn(float x) { return fn(x); } THUNK(fn, t_##fn)
#define MATH2(fn) double t_##fn(double x, double y) { return fn(x, y); } THUNK(fn, t_##fn)
#define MATH2F(fn) float t_##fn(float x, float y) { return fn(x, y); } THUNK(fn, t_##fn)
MATH1(acos); MATH1(asin); MATH1(atan); MATH1(ceil); MATH1(cos); MATH1(cosh); MATH1(exp); MATH1(floor);
MATH1(log); MATH1(log10); MATH1(rint); MATH1(sin); MATH1(sinh); MATH1(sqrt); MATH1(tan); MATH1(tanh);
MATH1(fabs); MATH1(round); MATH1(trunc); MATH1(exp2); MATH1(log2);
MATH1F(acosf); MATH1F(asinf); MATH1F(atanf); MATH1F(ceilf); MATH1F(cosf); MATH1F(expf); MATH1F(exp2f);
MATH1F(floorf); MATH1F(log10f); MATH1F(logf); MATH1F(sinf); MATH1F(sqrtf); MATH1F(tanf); MATH1F(fabsf);
MATH1F(roundf); MATH1F(truncf); MATH1F(log2f);
MATH2(atan2); MATH2(fmod); MATH2(pow); MATH2(hypot);
MATH2F(atan2f); MATH2F(fmodf); MATH2F(powf); MATH2F(hypotf);
s32 t_lrintf(float x) { return (s32)lrintf(x); }
THUNK(lrintf, t_lrintf);
s32 t_lrint(double x) { return (s32)lrint(x); }
THUNK(lrint, t_lrint);
double t_frexp(double x, G<int> e) { return frexp(x, e); }
THUNK(frexp, t_frexp);
double t_ldexp(double x, int e) { return ldexp(x, e); }
THUNK(ldexp, t_ldexp);
double t_scalbn(double x, int e) { return scalbn(x, e); }
THUNK(scalbn, t_scalbn);
double t_modf(double x, G<double> ip) {
    double i;
    double r = modf(x, &i);
    memcpy(ip.get(), &i, 8);  // guest doubles may be only 4-byte aligned
    return r;
}
THUNK(modf, t_modf);
float t_copysignf(float a, float b) { return copysignf(a, b); }
THUNK(copysignf, t_copysignf);
double t_copysign(double a, double b) { return copysign(a, b); }
THUNK(copysign, t_copysign);

// ---------------------------------------------------------------- process / misc
u32 t___errno() { return T().errno_addr; }
THUNK(__errno, t___errno);

[[noreturn]] void t_abort() {
    cpu::dump_state(T());
    fatal("guest called abort()");
}
THUNK(abort, t_abort);
void t_exit(int code) {
    LOGI("guest exit(%d)", code);
    _exit(code);
}
THUNK(exit, t_exit);
THUNK(_exit, t_exit);
void t___assert2(G<const char> file, int line, G<const char> func, G<const char> msg) {
    cpu::dump_state(T());
    fatal("guest assertion failed: %s:%d %s: %s", file.get(), line, func.get(), msg.get());
}
THUNK(__assert2, t___assert2);
void t___stack_chk_fail() {
    cpu::dump_state(T());
    fatal("guest stack corruption detected");
}
THUNK(__stack_chk_fail, t___stack_chk_fail);
void t___cxa_pure_virtual() {
    cpu::dump_state(T());
    fatal("guest pure virtual call");
}
THUNK(__cxa_pure_virtual, t___cxa_pure_virtual);
// Static destructors never need to run: the process is killed by Android.
int t___cxa_atexit(u32, u32, u32) { return 0; }
THUNK(__cxa_atexit, t___cxa_atexit);
THUNK(__aeabi_atexit, t___cxa_atexit);
void t___cxa_finalize(u32) {}
THUNK(__cxa_finalize, t___cxa_finalize);
int t_atexit(u32) { return 0; }
THUNK(atexit, t_atexit);

int t_raise(int sig) {
    LOGW("guest raise(%d)", sig);
    if (sig == SIGABRT || sig == SIGSEGV || sig == SIGKILL) t_abort();
    return 0;
}
THUNK(raise, t_raise);
int t_sigaction(int sig, u32, u32 old) {
    if (old) memset(g2h(old), 0, 16);
    LOGI("guest sigaction(%d) ignored", sig);
    return 0;
}
THUNK(sigaction, t_sigaction);
u32 t_bsd_signal(int sig, u32) {
    LOGI("guest signal(%d) ignored", sig);
    return 0;
}
THUNK(bsd_signal, t_bsd_signal);
THUNK(signal, t_bsd_signal);
int t_kill(int pid, int sig) { return check(kill(pid, sig)); }
THUNK(kill, t_kill);

std::mutex g_env_lock;
std::unordered_map<std::string, u32> g_env_cache;
u32 t_getenv(G<const char> name) {
    const char *v = getenv(name);
    if (!v) return 0;
    std::lock_guard<std::mutex> lk(g_env_lock);
    std::string key = std::string(name.get()) + "=" + v;
    auto it = g_env_cache.find(key);
    if (it != g_env_cache.end()) return it->second;
    u32 g = mem::hstrdup(v);
    g_env_cache[key] = g;
    return g;
}
THUNK(getenv, t_getenv);
int t_setenv(G<const char> n, G<const char> v, int o) { return check(setenv(n, v, o)); }
THUNK(setenv, t_setenv);
int t_unsetenv(G<const char> n) { return check(unsetenv(n)); }
THUNK(unsetenv, t_unsetenv);
int t_getpid() { return getpid(); }
THUNK(getpid, t_getpid);
int t_gettid() { return gettid(); }
THUNK(gettid, t_gettid);
u32 t_getuid() { return getuid(); }
THUNK(getuid, t_getuid);
THUNK(geteuid, t_getuid);
u32 t_getpwuid(u32 uid) {
    // bionic LP32 struct passwd: name, passwd, uid, gid, dir, shell
    static u32 pw = [] {
        u32 p = mem::halloc(24);
        wr<u32>(p + 0, mem::hstrdup("android"));
        wr<u32>(p + 4, mem::hstrdup(""));
        wr<u32>(p + 16, mem::hstrdup("/data"));
        wr<u32>(p + 20, mem::hstrdup("/system/bin/sh"));
        return p;
    }();
    wr<u32>(pw + 8, uid);
    wr<u32>(pw + 12, uid);
    return pw;
}
THUNK(getpwuid, t_getpwuid);
int t_gethostname(G<char> buf, u32 n) { return check(gethostname(buf, n)); }
THUNK(gethostname, t_gethostname);
s32 t_sysconf(int name) {
    if (name == _SC_PAGESIZE) return 4096;
    return (s32)sysconf(name);
}
THUNK(sysconf, t_sysconf);
int t_system(G<const char> cmd) {
    LOGW("guest system(\"%s\") refused", cmd ? cmd.get() : "");
    return -1;
}
THUNK(system, t_system);
u32 t_setlocale(int, u32) {
    static u32 c = mem::hstrdup("C");
    return c;
}
THUNK(setlocale, t_setlocale);
u32 t_strerror(int e) {
    static std::mutex lk;
    static std::unordered_map<int, u32> cache;
    std::lock_guard<std::mutex> g(lk);
    auto it = cache.find(e);
    if (it != cache.end()) return it->second;
    return cache[e] = mem::hstrdup(strerror(e));
}
THUNK(strerror, t_strerror);
int t_sched_yield() { return sched_yield(); }
THUNK(sched_yield, t_sched_yield);
int t_sched_get_priority_max(int) { return 0; }
THUNK(sched_get_priority_max, t_sched_get_priority_max);
THUNK(sched_get_priority_min, t_sched_get_priority_max);
int t___system_property_get(G<const char>, G<char> out) {
    *out.get() = 0;
    return 0;
}
THUNK(__system_property_get, t___system_property_get);

// ---------------------------------------------------------------- memory mapping
constexpr u32 G_MAP_FAILED = 0xffffffff;
void t_mmap(GuestThread &t) {
    u32 addr = t.r(0), len = t.r(1);
    int prot = (int)t.r(2), flags = (int)t.r(3);
    int fd = (int)t.arg(4);
    s32 off = (s32)t.arg(5);
    (void)prot;
    u32 a = mem::map(len, (flags & MAP_FIXED) ? addr : 0);
    if (len >= 8 * 1024 * 1024) LOGI("guest mmap(%u MB) -> 0x%08x", len >> 20, a);
    if (!a) {
        t.set_errno(ENOMEM);
        t.ret32(G_MAP_FAILED);
        return;
    }
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        // File mappings become private copies.
        ssize_t n = pread(fd, g2h(a), len, off);
        if (n < 0) LOGW("mmap: pread failed: %s", strerror(errno));
        else datapatch::apply(fd, off, g2h(a), n);
    }
    t.ret32(a);
}
THUNK_RAW(mmap, t_mmap);
int t_munmap(u32 addr, u32 len) {
    mem::unmap(addr, len);
    return 0;
}
THUNK(munmap, t_munmap);
void t_mremap(GuestThread &t) {
    u32 old = t.r(0), old_len = t.r(1), new_len = t.r(2);
    u32 a = mem::map(new_len);
    if (!a) {
        t.set_errno(ENOMEM);
        t.ret32(G_MAP_FAILED);
        return;
    }
    memcpy(g2h(a), g2h(old), old_len < new_len ? old_len : new_len);
    mem::unmap(old, old_len);
    t.ret32(a);
}
THUNK_RAW(mremap, t_mremap);
int t_mprotect(u32, u32, int) { return 0; }
THUNK(mprotect, t_mprotect);
int t_madvise(u32, u32, int) { return 0; }
THUNK(madvise, t_madvise);

// ---------------------------------------------------------------- dynamic linking
constexpr u32 G_RTLD_DEFAULT = 0xffffffff;
constexpr u32 HOST_LIB_HANDLE = 0xfffffff0;  // a system library that only exists as thunks
thread_local u32 t_dlerror_msg;
void set_dlerror(const std::string &msg) {
    if (t_dlerror_msg) mem::hfree(t_dlerror_msg);
    t_dlerror_msg = mem::hstrdup(msg.c_str());
}
u32 t_dlopen(G<const char> name, int) {
    if (!name) return G_RTLD_DEFAULT;
    std::string n = name.get();
    auto slash = n.rfind('/');
    if (slash != std::string::npos) n = n.substr(slash + 1);
    if (Module *m = loader::find(n)) return m->base | 1;  // handle = module base, tagged
    static const char *host_libs[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libGLESv2.so",
                                      "libGLESv1_CM.so", "libEGL.so", "libandroid.so", "libstdc++.so"};
    // libOpenSLES.so is deliberately absent: FMOD then falls back to its Java AudioTrack output.
    for (const char *h : host_libs)
        if (n == h) return HOST_LIB_HANDLE;
    LOGW("guest dlopen(%s) failed", name.get());
    set_dlerror("library \"" + n + "\" not found");
    return 0;
}
THUNK(dlopen, t_dlopen);
u32 t_dlsym(u32 handle, G<const char> sym) {
    u32 a = 0;
    if (handle != G_RTLD_DEFAULT && handle != HOST_LIB_HANDLE && (handle & 1)) {
        if (Module *m = loader::find_by_addr(handle & ~1u)) a = loader::lookup_in(m, sym);
    } else {
        a = loader::lookup(sym);
    }
    if (!a) {
        LOGW("guest dlsym(%s) failed", sym.get());
        set_dlerror(std::string("undefined symbol: ") + sym.get());
    }
    return a;
}
THUNK(dlsym, t_dlsym);
int t_dlclose(u32) { return 0; }
THUNK(dlclose, t_dlclose);
u32 t_dlerror() {
    u32 m = t_dlerror_msg;
    t_dlerror_msg = 0;  // leaked once; dlerror is rare
    return m;
}
THUNK(dlerror, t_dlerror);

// _Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *pcount)
u32 t___gnu_Unwind_Find_exidx(u32 pc, G<s32> count) {
    Module *m = loader::find_by_addr(pc);
    if (!m || !m->exidx) {
        *count.get() = 0;
        return 0;
    }
    *count.get() = (s32)m->exidx_count;
    return m->exidx;
}
THUNK(__gnu_Unwind_Find_exidx, t___gnu_Unwind_Find_exidx);

// ---------------------------------------------------------------- logging
int t___android_log_write(int prio, G<const char> tag, G<const char> msg) {
    return __android_log_write(prio, tag ? tag.get() : "cq-guest", msg ? msg.get() : "");
}
THUNK(__android_log_write, t___android_log_write);

// ---------------------------------------------------------------- networking (disabled)
int t_socket(int, int, int) { return fail(EACCES); }
THUNK(socket, t_socket);
int t_sock_fail3(int, u32, u32) { return fail(EBADF); }
THUNK(bind, t_sock_fail3);
THUNK(connect, t_sock_fail3);
THUNK(accept, t_sock_fail3);
THUNK(getsockname, t_sock_fail3);
int t_listen(int, int) { return fail(EBADF); }
THUNK(listen, t_listen);
s32 t_send(int, u32, u32, int) { return fail(EBADF); }
THUNK(send, t_send);
THUNK(recv, t_send);
void t_sendto(GuestThread &t) { t.ret32((u32)fail(EBADF)); }
THUNK_RAW(sendto, t_sendto);
THUNK_RAW(recvfrom, t_sendto);
int t_setsockopt(int, int, int, u32, u32) { return fail(EBADF); }
THUNK(setsockopt, t_setsockopt);
u32 t_gethostbyname(G<const char>) { return 0; }
THUNK(gethostbyname, t_gethostbyname);
u32 t_inet_addr(G<const char> s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0xffffffff;
    return a | (b << 8) | (c << 16) | (d << 24);
}
THUNK(inet_addr, t_inet_addr);
u32 t_inet_ntoa(u32 in) {
    thread_local u32 buf = mem::halloc(16);
    snprintf((char *)g2h(buf), 16, "%u.%u.%u.%u", in & 255, (in >> 8) & 255, (in >> 16) & 255, in >> 24);
    return buf;
}
THUNK(inet_ntoa, t_inet_ntoa);

// ---------------------------------------------------------------- added for Moai games (MMoJ)

// time
u32 t_gmtime_r(u32 tp, u32 out) {
    time_t t = rd<s32>(tp);
    struct tm h;
    gmtime_r(&t, &h);
    put_tm(out, h);
    return out;
}
THUNK(gmtime_r, t_gmtime_r);

// strings
u32 t_strtok_r(u32 str, G<const char> delim, u32 save) {
    u32 p = str ? str : rd<u32>(save);
    if (!p) return 0;
    const char *d = delim;
    while (rd<char>(p) && strchr(d, rd<char>(p))) p++;  // skip leading delimiters
    if (!rd<char>(p)) {
        wr<u32>(save, 0);
        return 0;
    }
    u32 tok = p;
    while (rd<char>(p) && !strchr(d, rd<char>(p))) p++;
    if (rd<char>(p)) {
        wr<char>(p, 0);
        wr<u32>(save, p + 1);
    } else {
        wr<u32>(save, 0);
    }
    return tok;
}
THUNK(strtok_r, t_strtok_r);
u32 t_strxfrm(G<char> dst, G<const char> src, u32 n) { return (u32)strxfrm(dst.a ? dst.get() : nullptr, src, n); }
THUNK(strxfrm, t_strxfrm);
u32 t_basename(G<const char> path) {
    thread_local u32 buf = mem::halloc(4096);
    std::string p = path.a ? path.get() : "";
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t i = p.rfind('/');
    std::string b = p.empty() ? "." : (i == std::string::npos || p == "/") ? p : p.substr(i + 1);
    snprintf((char *)g2h(buf), 4096, "%s", b.c_str());
    return buf;
}
THUNK(basename, t_basename);

// wide characters (wchar_t is 32 bits on both sides; mbstate_t is 4 bytes in the guest)
struct GuestMbstate {
    u32 g;
    mbstate_t h{};
    explicit GuestMbstate(u32 g) : g(g) { if (g) memcpy(&h, g2h(g), 4); }
    ~GuestMbstate() { if (g) memcpy(g2h(g), &h, 4); }
};
u32 t_mbrtowc(G<wchar_t> pwc, G<const char> s, u32 n, u32 ps) {
    static thread_local u32 internal = mem::halloc(4);
    GuestMbstate st(ps ? ps : internal);
    return (u32)mbrtowc(pwc.a ? pwc.get() : nullptr, s.a ? s.get() : nullptr, n, &st.h);
}
THUNK(mbrtowc, t_mbrtowc);
u32 t_wcrtomb(G<char> s, s32 wc, u32 ps) {
    static thread_local u32 internal = mem::halloc(4);
    GuestMbstate st(ps ? ps : internal);
    return (u32)wcrtomb(s.a ? s.get() : nullptr, (wchar_t)wc, &st.h);
}
THUNK(wcrtomb, t_wcrtomb);
u32 t_btowc(int c) { return (u32)btowc(c); } THUNK(btowc, t_btowc);
int t_wctob(u32 c) { return wctob((wint_t)c); } THUNK(wctob, t_wctob);
u32 t_towlower(u32 c) { return (u32)towlower((wint_t)c); } THUNK(towlower, t_towlower);
u32 t_towupper(u32 c) { return (u32)towupper((wint_t)c); } THUNK(towupper, t_towupper);
u32 t_wctype(G<const char> name) { return (u32)wctype(name); } THUNK(wctype, t_wctype);
int t_iswctype(u32 c, u32 type) { return iswctype((wint_t)c, (wctype_t)type); } THUNK(iswctype, t_iswctype);
int t_wcscoll(G<const wchar_t> a, G<const wchar_t> b) { return wcscoll(a, b); } THUNK(wcscoll, t_wcscoll);
u32 t_wcsxfrm(G<wchar_t> d, G<const wchar_t> s, u32 n) { return (u32)wcsxfrm(d.a ? d.get() : nullptr, s, n); }
THUNK(wcsxfrm, t_wcsxfrm);
u32 t_wcsftime(G<wchar_t> s, u32 max, G<const wchar_t> fmt, u32 tmp) {
    struct tm t = get_tm(tmp);
    return (u32)wcsftime(s, max, fmt, &t);
}
THUNK(wcsftime, t_wcsftime);
u32 t_wmemchr(u32 s, s32 c, u32 n) {
    const wchar_t *p = wmemchr((const wchar_t *)g2h(s), (wchar_t)c, n);
    return p ? h2g(p) : 0;
}
THUNK(wmemchr, t_wmemchr);
int t_wmemcmp(G<const wchar_t> a, G<const wchar_t> b, u32 n) { return wmemcmp(a, b, n); } THUNK(wmemcmp, t_wmemcmp);
u32 t_wmemcpy(u32 d, G<const wchar_t> s, u32 n) { wmemcpy((wchar_t *)g2h(d), s, n); return d; } THUNK(wmemcpy, t_wmemcpy);
u32 t_wmemmove(u32 d, G<const wchar_t> s, u32 n) { wmemmove((wchar_t *)g2h(d), s, n); return d; } THUNK(wmemmove, t_wmemmove);
u32 t_wmemset(u32 d, s32 c, u32 n) { wmemset((wchar_t *)g2h(d), (wchar_t)c, n); return d; } THUNK(wmemset, t_wmemset);
u32 t_getwc(u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return (u32)WEOF;
    wint_t r = fgetwc(f);
    sync_flags(gf, f);
    return (u32)r;
}
THUNK(getwc, t_getwc);
u32 t_putwc(u32 c, u32 gf) {
    FILE *f = hfile(gf);
    return f ? (u32)fputwc((wchar_t)c, f) : (u32)WEOF;
}
THUNK(putwc, t_putwc);
u32 t_ungetwc(u32 c, u32 gf) {
    FILE *f = hfile(gf);
    return f ? (u32)ungetwc((wint_t)c, f) : (u32)WEOF;
}
THUNK(ungetwc, t_ungetwc);

// stdio
THUNK(putc, t_fputc);
void t_clearerr(u32 gf) {
    if (FILE *f = hfile(gf)) {
        clearerr(f);
        sync_flags(gf, f);
    }
}
THUNK(clearerr, t_clearerr);
void t_setbuf(u32, u32) {}  // host buffering is kept
THUNK(setbuf, t_setbuf);
u32 t_fdopen(int fd, G<const char> mode) { return guest_file(fdopen(fd, mode)); }
THUNK(fdopen, t_fdopen);
u32 t_freopen(G<const char> path, G<const char> mode, u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF), 0;
    std::string p = path.a ? paths::translate(path) : std::string();
    if (!freopen(path.a ? p.c_str() : nullptr, mode, f)) return fail(), 0;
    wr<s16>(gf + 14, (s16)fileno(f));
    return gf;
}
THUNK(freopen, t_freopen);
u32 t_tmpfile() { return guest_file(tmpfile()); }
THUNK(tmpfile, t_tmpfile);
u32 t_tmpnam(u32 buf) {
    thread_local u32 own = mem::halloc(64);
    static std::atomic<int> n{0};
    u32 out = buf ? buf : own;
    snprintf((char *)g2h(out), 20, "/data/local/tmp/t%d", n++);
    return out;
}
THUNK(tmpnam, t_tmpnam);
u32 t_popen(G<const char> cmd, G<const char> mode) { return guest_file(popen(cmd, mode)); }
THUNK(popen, t_popen);
int t_pclose(u32 gf) {
    FILE *f;
    {
        std::lock_guard<std::mutex> lk(g_files_lock);
        auto it = g_files.find(gf);
        if (it == g_files.end()) return fail(EBADF);
        f = it->second;
        g_files.erase(it);
    }
    mem::hfree(gf);
    return pclose(f);
}
THUNK(pclose, t_pclose);

// process / file system
u32 t_getcwd(u32 buf, u32 size) {
    char tmp[4096];
    if (!getcwd(tmp, sizeof tmp)) return fail(), 0;
    size_t n = strlen(tmp) + 1;
    if (!buf) {
        buf = mem::halloc(size > n ? size : (u32)n);
    } else if (n > size) {
        return fail(ERANGE), 0;
    }
    memcpy(g2h(buf), tmp, n);
    return buf;
}
THUNK(getcwd, t_getcwd);
int t_chdir(G<const char> path) { return check(chdir(paths::translate(path).c_str())); }
THUNK(chdir, t_chdir);
u32 t_getgid() { return getgid(); } THUNK(getgid, t_getgid);
u32 t_getegid() { return getegid(); } THUNK(getegid, t_getegid);
u32 t_alarm(u32 s) { return alarm(s); } THUNK(alarm, t_alarm);
// struct pollfd is {int fd; short events, revents;} on both sides.
int t_poll(G<struct pollfd> fds, u32 n, int timeout) { return check(poll(fds, n, timeout)); }
THUNK(poll, t_poll);

// networking: sockets are not supported (the game only uses them for analytics and store features)
int t_getaddrinfo(u32, u32, u32, u32 res) {
    if (res) wr<u32>(res, 0);
    return 4;  // EAI_FAIL
}
THUNK(getaddrinfo, t_getaddrinfo);
void t_freeaddrinfo(u32) {}
THUNK(freeaddrinfo, t_freeaddrinfo);
int t_getsockopt(int, int, int, u32, u32) { return fail(EBADF); }
THUNK(getsockopt, t_getsockopt);
int t_shutdown(int, int) { return fail(EBADF); }
THUNK(shutdown, t_shutdown);
u32 t_inet_ntop(int af, G<const void> src, u32 dst, u32 size) {
    if (af == 2 /* AF_INET */) return inet_ntop(AF_INET, src, (char *)g2h(dst), size) ? dst : 0;
    return fail(EAFNOSUPPORT), 0;
}
THUNK(inet_ntop, t_inet_ntop);
int t_inet_pton(int af, G<const char> src, G<void> dst) {
    if (af == 2) return inet_pton(AF_INET, src, dst);
    return fail(EAFNOSUPPORT);
}
THUNK(inet_pton, t_inet_pton);
}  // namespace

namespace libc_host {
void init() {
    g_sF = loader::lookup("__sF", false);
    if (!g_sF) fatal("guest runtime does not define __sF");
    for (int i = 0; i < 3; i++) wr<s16>(g_sF + i * GFILE_SIZE + 14, (s16)i);
}
}  // namespace libc_host
