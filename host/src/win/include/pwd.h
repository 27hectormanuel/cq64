// SPDX-License-Identifier: CC0-1.0
#pragma once
#include <sys/types.h>
struct passwd {
    char *pw_name, *pw_passwd;
    unsigned pw_uid, pw_gid;
    char *pw_dir, *pw_shell;
};
#ifdef __cplusplus
extern "C" {
#endif
struct passwd *getpwuid(unsigned uid);
#ifdef __cplusplus
}
#endif
