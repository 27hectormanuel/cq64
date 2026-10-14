// SPDX-License-Identifier: CC0-1.0
#pragma once
#define FIONREAD 0x541B
#define FIONBIO 0x5421
#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int fd, unsigned long req, ...);
#ifdef __cplusplus
}
#endif
