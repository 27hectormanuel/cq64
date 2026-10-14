// SPDX-License-Identifier: CC0-1.0
// Windows build: the POSIX and Android functions declared by the headers in win/include, on
// Win32 and MinGW's C runtime.
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <sys/utime.h>

#include <android/log.h>
#include <android/native_window.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/uio.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {
std::mutex g_log_lock;
FILE *g_log_file;

const char kLevel[] = "??VDIWEFS";
}  // namespace

extern "C" {

// ---- logging: stderr and cq64.log next to the program
void cq_log_open(const char *path) {
    std::lock_guard<std::mutex> lk(g_log_lock);
    g_log_file = fopen(path, "w");
}

int __android_log_write(int prio, const char *tag, const char *text) {
    std::lock_guard<std::mutex> lk(g_log_lock);
    char lvl = prio >= 0 && prio <= 8 ? kLevel[prio] : '?';
    SYSTEMTIME t;
    GetLocalTime(&t);
    for (FILE *f : {stderr, g_log_file}) {
        if (!f) continue;
        fprintf(f, "%02d:%02d:%02d.%03d %5lu %c %s: %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentThreadId(), lvl, tag ? tag : "", text);
        fflush(f);
    }
    return 1;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

// ---- unistd and friends
int gettid(void) { return (int)GetCurrentThreadId(); }

namespace {
std::mutex g_pio_lock;  // pread/pwrite move the shared file position and put it back
}

ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    std::lock_guard<std::mutex> lk(g_pio_lock);
    __int64 old = _lseeki64(fd, 0, SEEK_CUR);
    if (old < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return -1;
    size_t done = 0;
    while (done < n) {
        unsigned chunk = (unsigned)std::min<size_t>(n - done, 1u << 30);
        int r = _read(fd, (char *)buf + done, chunk);
        if (r < 0) {
            _lseeki64(fd, old, SEEK_SET);
            return -1;
        }
        if (r == 0) break;
        done += r;
    }
    _lseeki64(fd, old, SEEK_SET);
    return (ssize_t)done;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    std::lock_guard<std::mutex> lk(g_pio_lock);
    __int64 old = _lseeki64(fd, 0, SEEK_CUR);
    if (old < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _write(fd, buf, (unsigned)n);
    _lseeki64(fd, old, SEEK_SET);
    return r;
}

int fsync(int fd) { return _commit(fd); }
unsigned getuid(void) { return 10000; }
unsigned getgid(void) { return 10000; }
unsigned getegid(void) { return 10000; }
unsigned alarm(unsigned) { return 0; }

int kill(int pid, int sig) {
    if (pid == _getpid()) return raise(sig);
    errno = EPERM;
    return -1;
}

long sysconf(int name) {
    switch (name) {
    case _SC_CLK_TCK: return 100;
    case _SC_PAGESIZE:
    case _SC_PAGE_SIZE: return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return si.dwNumberOfProcessors;
    }
    default: errno = EINVAL; return -1;
    }
}

int gethostname(char *name, int len) {
    snprintf(name, len, "cq64");
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
int unsetenv(const char *name) { return _putenv_s(name, "") == 0 ? 0 : -1; }

int cq_mkdir(const char *path, int) { return _mkdir(path); }

int cq_rename(const char *from, const char *to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    DWORD e = GetLastError();
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
          : e == ERROR_ACCESS_DENIED ? EACCES
          : EIO;
    return -1;
}

int utimes(const char *path, const struct timeval tv[2]) {
    if (!tv) return _utime(path, nullptr);
    struct _utimbuf u{(time_t)tv[0].tv_sec, (time_t)tv[1].tv_sec};
    return _utime(path, &u);
}

// Windows errno -> Linux errno (identical below 35).
int cq_errno_to_linux(int e) {
    switch (e) {
    case EDEADLK: return 35;
    case ENAMETOOLONG: return 36;
    case ENOLCK: return 37;
    case ENOSYS: return 38;
    case ENOTEMPTY: return 39;
    case EILSEQ: return 84;
    case EWOULDBLOCK: return 11;
    case ETIMEDOUT: return 110;
    case EINPROGRESS: return 115;
    case EALREADY: return 114;
    case ECONNREFUSED: return 111;
    case ECONNRESET: return 104;
    case ENOTSUP: return 95;
    case EOPNOTSUPP: return 95;
    case EOVERFLOW: return 75;
    case ECANCELED: return 125;
    default: return e;
    }
}

int fcntl(int fd, int cmd, ...) {
    switch (cmd) {
    case F_GETFL: return O_RDWR;
    case F_SETFL:
    case F_GETFD:
    case F_SETFD: return 0;
    case F_DUPFD: return _dup(fd);
    default: errno = EINVAL; return -1;
    }
}

int ioctl(int fd, unsigned long req, ...) {
    if (req == FIONREAD) {
        va_list ap;
        va_start(ap, req);
        int *out = va_arg(ap, int *);
        va_end(ap);
        __int64 pos = _lseeki64(fd, 0, SEEK_CUR), end = _filelengthi64(fd);
        if (pos < 0 || end < 0) return -1;
        *out = (int)std::min<__int64>(end - pos, 0x7fffffff);
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

// The guest only polls/selects on descriptors that never become ready here (no sockets, no
// pipes): sleep out the timeout.
int poll(struct pollfd *fds, unsigned long n, int timeout) {
    for (unsigned long i = 0; i < n; i++) fds[i].revents = 0;
    if (timeout > 0) Sleep((DWORD)timeout);
    return 0;
}

int select(int, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
    for (fd_set *s : {r, w, e})
        if (s) memset(s, 0, sizeof *s);
    if (tv) Sleep((DWORD)(tv->tv_sec * 1000 + tv->tv_usec / 1000));
    return 0;
}

struct passwd *getpwuid(unsigned uid) {
    static char name[] = "cq64", dir[] = "/data/data/com.doublefine.cq", shell[] = "/system/bin/sh", pw[] = "";
    static struct passwd p{name, pw, 0, 0, dir, shell};
    p.pw_uid = p.pw_gid = uid;
    return &p;
}

const char *inet_ntop(int af, const void *src, char *dst, unsigned size) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return nullptr;
    }
    const unsigned char *b = (const unsigned char *)src;
    snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return dst;
}

int inet_pton(int af, const char *src, void *dst) {
    unsigned a, b, c, d;
    if (af != AF_INET || sscanf(src, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    unsigned char *o = (unsigned char *)dst;
    o[0] = (unsigned char)a, o[1] = (unsigned char)b, o[2] = (unsigned char)c, o[3] = (unsigned char)d;
    return 1;
}

// ---- memory: the host's anonymous mappings (the guest address space) on VirtualAlloc
namespace {
DWORD page_protect(int prot) {
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
}  // namespace

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t) {
    if (!(flags & MAP_ANONYMOUS) || fd != -1 || addr) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    // Committed up front: pages take memory only once touched, and the commit charge is what
    // lets the guest grow without a page fault handler of our own.
    void *p = VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, page_protect(prot));
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return p;
}

int munmap(void *addr, size_t) { return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1; }

int mprotect(void *addr, size_t len, int prot) {
    DWORD old;
    return VirtualProtect(addr, len, page_protect(prot), &old) ? 0 : -1;
}

int madvise(void *addr, size_t len, int advice) {
    if (advice != MADV_DONTNEED) return 0;
    // Pages must read back as zero afterwards: decommit and commit again.
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof mbi)) return -1;
    if (!VirtualFree(addr, len, MEM_DECOMMIT)) return -1;
    return VirtualAlloc(addr, len, MEM_COMMIT, mbi.Protect ? mbi.Protect : PAGE_READWRITE) ? 0 : -1;
}

ssize_t process_vm_readv(int, const struct iovec *local, unsigned long, const struct iovec *remote, unsigned long,
                         unsigned long) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), remote->iov_base, local->iov_base,
                           std::min(local->iov_len, remote->iov_len), &got)) {
        errno = EFAULT;
        return -1;
    }
    return (ssize_t)got;
}

// ---- dlfcn
namespace {
thread_local std::string t_dlerror;
}

int dladdr(const void *addr, Dl_info *info) {
    static char path[MAX_PATH];
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m))
        return 0;
    GetModuleFileNameA(m, path, sizeof path);
    for (char *p = path; *p; p++)
        if (*p == '\\') *p = '/';
    *info = Dl_info{path, (void *)m, nullptr, nullptr};
    return 1;
}

void *dlopen(const char *name, int) {
    HMODULE m = name ? LoadLibraryA(name) : GetModuleHandleA(nullptr);
    if (!m) t_dlerror = std::string("cannot load ") + (name ? name : "(self)");
    return (void *)m;
}

void *dlsym(void *handle, const char *name) {
    void *p = (void *)GetProcAddress(handle ? (HMODULE)handle : GetModuleHandleA(nullptr), name);
    if (!p) t_dlerror = std::string("symbol not found: ") + name;
    return p;
}

int dlclose(void *handle) { return handle && FreeLibrary((HMODULE)handle) ? 0 : -1; }

const char *dlerror(void) {
    static thread_local std::string last;
    if (t_dlerror.empty()) return nullptr;
    last.swap(t_dlerror);
    t_dlerror.clear();
    return last.c_str();
}

}  // extern "C"
