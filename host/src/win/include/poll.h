// SPDX-License-Identifier: CC0-1.0
#pragma once
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
struct pollfd {
    int fd;
    short events;
    short revents;
};
#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd *fds, unsigned long n, int timeout);
#ifdef __cplusplus
}
#endif
