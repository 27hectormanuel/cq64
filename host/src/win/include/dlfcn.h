// SPDX-License-Identifier: CC0-1.0
// Windows build: dladdr (the program's own path) and dlopen/dlsym on LoadLibrary.
#pragma once
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void *)0)
typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;
#ifdef __cplusplus
extern "C" {
#endif
int dladdr(const void *addr, Dl_info *info);
void *dlopen(const char *name, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
const char *dlerror(void);
#ifdef __cplusplus
}
#endif
