// SPDX-License-Identifier: CC0-1.0
// Windows build: the guest's (bionic LP32) fd_set, 1024 bits; select() only sleeps out the timeout.
#pragma once
#include <sys/time.h>
typedef struct {
    unsigned long fds_bits[1024 / (8 * sizeof(unsigned long))];
} fd_set;
#ifdef __cplusplus
extern "C" {
#endif
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv);
#ifdef __cplusplus
}
#endif
