// SPDX-License-Identifier: CC0-1.0
#pragma once
#include <stdint.h>
#include <sys/types.h>
#define AF_INET 2
#define AF_INET6 23
#ifdef __cplusplus
extern "C" {
#endif
const char *inet_ntop(int af, const void *src, char *dst, unsigned size);
int inet_pton(int af, const char *src, void *dst);
#ifdef __cplusplus
}
#endif
