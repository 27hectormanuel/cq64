// SPDX-License-Identifier: CC0-1.0
// Windows build: Android's logging API, written to stderr and cq64.log (posix_win.cpp).
#pragma once
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { ANDROID_LOG_UNKNOWN, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
       ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT };
int __android_log_write(int prio, const char *tag, const char *text);
int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);
#ifdef __cplusplus
}
#endif
