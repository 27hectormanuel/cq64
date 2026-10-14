// SPDX-License-Identifier: CC0-1.0
#pragma once
#include <stddef.h>
#include <sys/types.h>
struct iovec {
    void *iov_base;
    size_t iov_len;
};
#ifdef __cplusplus
extern "C" {
#endif
// Only reads from the calling process (ReadProcessMemory), which is all luahints needs.
ssize_t process_vm_readv(int pid, const struct iovec *local, unsigned long nlocal, const struct iovec *remote,
                         unsigned long nremote, unsigned long flags);
#ifdef __cplusplus
}
#endif
