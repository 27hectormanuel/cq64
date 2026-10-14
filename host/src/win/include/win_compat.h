// SPDX-License-Identifier: CC0-1.0
// Force-included into every host source of the Windows build (-include): the POSIX functions
// the host uses that MinGW's C runtime lacks or spells differently. Implemented in posix_win.cpp.
// windows.h is deliberately not included here (its macros clash with ordinary names).
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <direct.h>  // before the mkdir macro below
#include <io.h>
#include <unistd.h>

#ifdef __cplusplus
#include <cstdlib>
extern "C" {
#endif
int gettid(void);
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int fsync(int fd);
unsigned getuid(void);
int kill(int pid, int sig);
long sysconf(int name);
int gethostname(char *name, int len);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int cq_mkdir(const char *path, int mode);
int cq_rename(const char *from, const char *to);
int cq_errno_to_linux(int e);
unsigned getgid(void);
unsigned getegid(void);
unsigned alarm(unsigned seconds);
#ifdef __cplusplus
}
#endif

// POSIX mkdir takes a mode; rename replaces an existing target (MoveFileEx).
#define mkdir(path, mode) cq_mkdir(path, mode)
#define rename(from, to) cq_rename(from, to)
#define SIGKILL 9

// The guest's (bionic) sysconf names.
#define _SC_CLK_TCK 0x0006
#define _SC_PAGESIZE 0x0027
#define _SC_PAGE_SIZE 0x0028
#define _SC_NPROCESSORS_CONF 0x0060
#define _SC_NPROCESSORS_ONLN 0x0061

#include <sys/time.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
int utimes(const char *path, const struct timeval tv[2]);
#ifdef __cplusplus
}
#endif
static inline struct tm *localtime_r(const time_t *t, struct tm *r) { return localtime_s(r, t) ? NULL : r; }
static inline struct tm *gmtime_r(const time_t *t, struct tm *r) { return gmtime_s(r, t) ? NULL : r; }
