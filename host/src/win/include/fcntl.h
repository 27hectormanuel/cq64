// SPDX-License-Identifier: CC0-1.0
// Windows build: MinGW's fcntl.h plus the POSIX flags and fcntl() it lacks.
#pragma once
#include_next <fcntl.h>
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#ifdef __cplusplus
extern "C" {
#endif
int fcntl(int fd, int cmd, ...);
#ifdef __cplusplus
}
#endif
