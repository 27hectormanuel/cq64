// SPDX-License-Identifier: GPL-3.0-or-later
// Guest-side formatting (printf/scanf families), ctype tables and libc data objects.
// Actual I/O goes through host thunks (fwrite, fgetc, ungetc, __android_log_write).
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define EXPORT __attribute__((visibility("default")))

#define STB_SPRINTF_IMPLEMENTATION
#define STB_SPRINTF_STATIC
#define STB_SPRINTF_NOUNALIGNED
#include "../third_party/misc/stb_sprintf.h"

typedef struct GFILE GFILE;
// Bionic (LP32) FILE is 84 bytes; only the host interprets the contents.
EXPORT unsigned char __sF[3][84];

extern size_t fwrite(const void *, size_t, size_t, GFILE *);
extern int fgetc(GFILE *);
extern int ungetc(int, GFILE *);
extern int __android_log_write(int prio, const char *tag, const char *msg);
extern void *malloc(size_t);
extern void free(void *);
extern double strtod(const char *, char **);
extern unsigned long long strtoull(const char *, char **, int);
extern long long strtoll(const char *, char **, int);
extern size_t strlen(const char *);

#define STDOUT ((GFILE *)__sF[1])

EXPORT int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return stbsp_vsnprintf(buf, (int)n, fmt, ap); }
EXPORT int vsprintf(char *buf, const char *fmt, va_list ap) { return stbsp_vsprintf(buf, fmt, ap); }
EXPORT int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = stbsp_vsnprintf(buf, (int)n, fmt, ap);
    va_end(ap);
    return r;
}
EXPORT int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = stbsp_vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

EXPORT int vfprintf(GFILE *f, const char *fmt, va_list ap) {
    char small[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = stbsp_vsnprintf(small, sizeof small, fmt, ap);
    if (n < 0) { va_end(ap2); return n; }
    if ((size_t)n < sizeof small) {
        fwrite(small, 1, n, f);
    } else {
        char *big = malloc(n + 1);
        stbsp_vsnprintf(big, n + 1, fmt, ap2);
        fwrite(big, 1, n, f);
        free(big);
    }
    va_end(ap2);
    return n;
}
EXPORT int fprintf(GFILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
EXPORT int vprintf(const char *fmt, va_list ap) { return vfprintf(STDOUT, fmt, ap); }
EXPORT int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(STDOUT, fmt, ap);
    va_end(ap);
    return r;
}
EXPORT int puts(const char *s) {
    fwrite(s, 1, strlen(s), STDOUT);
    fwrite("\n", 1, 1, STDOUT);
    return 1;
}
EXPORT int putchar(int c) {
    char ch = (char)c;
    fwrite(&ch, 1, 1, STDOUT);
    return (unsigned char)c;
}

EXPORT int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    char buf[1024];
    stbsp_vsnprintf(buf, sizeof buf, fmt, ap);
    return __android_log_write(prio, tag, buf);
}
EXPORT int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

// ---- scanf ----
typedef struct {
    int (*get)(void *);
    void (*unget)(int, void *);
    void *ctx;
    int count;
} Src;

typedef struct { const char *p; } StrCtx;
static int str_get(void *c) {
    StrCtx *s = c;
    if (!*s->p) return -1;
    return (unsigned char)*s->p++;
}
static void str_unget(int ch, void *c) {
    StrCtx *s = c;
    if (ch >= 0) s->p--;
}
static int file_get(void *c) { return fgetc((GFILE *)c); }
static void file_unget(int ch, void *c) { if (ch >= 0) ungetc(ch, (GFILE *)c); }

static int sget(Src *s) { int c = s->get(s->ctx); if (c >= 0) s->count++; return c; }
static void sunget(Src *s, int c) { if (c >= 0) { s->unget(c, s->ctx); s->count--; } }
static int isspace_(int c) { return c == ' ' || (c >= 9 && c <= 13); }

static int vscan(Src *s, const char *fmt, va_list ap) {
    int assigned = 0, c;
    char tmp[512];
    for (; *fmt; fmt++) {
        if (isspace_((unsigned char)*fmt)) {
            while (isspace_(c = sget(s))) {}
            sunget(s, c);
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            c = sget(s);
            if (c != (unsigned char)*fmt) { sunget(s, c); return assigned ? assigned : (c < 0 ? -1 : 0); }
            continue;
        }
        fmt++;
        int suppress = 0, width = 0, len = 0;  // len: -2 hh, -1 h, 0 int, 1 l, 2 ll
        if (*fmt == '*') { suppress = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        for (;;) {
            if (*fmt == 'h') { len--; fmt++; }
            else if (*fmt == 'l') { len++; fmt++; }
            else if (*fmt == 'L' || *fmt == 'q') { len = 2; fmt++; }
            else if (*fmt == 'j') { len = 2; fmt++; }
            else if (*fmt == 'z' || *fmt == 't') { len = 0; fmt++; }
            else break;
        }
        char conv = *fmt;
        if (!conv) break;
        if (conv == 'n') {
            if (!suppress) *va_arg(ap, int *) = s->count;
            continue;
        }
        if (conv != 'c' && conv != '[') {
            while (isspace_(c = sget(s))) {}
            sunget(s, c);
        }
        if (width <= 0 || width > (int)sizeof tmp - 1) width = (conv == 'c') ? 1 : (int)sizeof tmp - 1;
        if (conv == 'c') {
            char *out = suppress ? 0 : va_arg(ap, char *);
            for (int i = 0; i < width; i++) {
                c = sget(s);
                if (c < 0) return assigned ? assigned : -1;
                if (out) out[i] = (char)c;
            }
            if (!suppress) assigned++;
            continue;
        }
        if (conv == 's' || conv == '[') {
            unsigned char set[256];
            int invert = 0;
            if (conv == '[') {
                fmt++;
                if (*fmt == '^') { invert = 1; fmt++; }
                for (int i = 0; i < 256; i++) set[i] = 0;
                if (*fmt == ']') { set[']'] = 1; fmt++; }
                while (*fmt && *fmt != ']') {
                    if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                        for (int ch = (unsigned char)fmt[0]; ch <= (unsigned char)fmt[2]; ch++) set[ch] = 1;
                        fmt += 3;
                    } else set[(unsigned char)*fmt++] = 1;
                }
            }
            char *out = suppress ? 0 : va_arg(ap, char *);
            int n = 0;
            while (n < width) {
                c = sget(s);
                if (c < 0) break;
                int ok = conv == 's' ? !isspace_(c) : (set[c] ^ invert);
                if (!ok) { sunget(s, c); break; }
                if (out) out[n] = (char)c;
                n++;
            }
            if (n == 0) return assigned ? assigned : (c < 0 ? -1 : 0);
            if (out) { out[n] = 0; assigned++; }
            continue;
        }
        // numeric: collect the candidate characters into tmp
        int n = 0;
        int isfloat = (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a');
        int base = conv == 'd' || conv == 'u' ? 10 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : 0;
        while (n < width) {
            c = sget(s);
            if (c < 0) break;
            int ok;
            if (isfloat) ok = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-' ||
                              c == 'n' || c == 'a' || c == 'i' || c == 'f' || c == 'N' || c == 'A' || c == 'I' || c == 'F';
            else ok = (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f' && base != 10 && base != 8) ||
                      ((c | 32) == 'x' && base != 10 && base != 8) || ((c == '+' || c == '-') && n == 0);
            if (isfloat && (c == '+' || c == '-') && n > 0 && (tmp[n - 1] | 32) != 'e') ok = 0;
            if (!ok) { sunget(s, c); break; }
            tmp[n++] = (char)c;
        }
        tmp[n] = 0;
        if (n == 0) return assigned ? assigned : (c < 0 ? -1 : 0);
        char *end;
        if (isfloat) {
            double v = strtod(tmp, &end);
            if (end == tmp) return assigned;
            // unread anything strtod did not consume
            for (int i = n - 1; tmp + i >= end; i--) sunget(s, (unsigned char)tmp[i]);
            if (!suppress) {
                if (len >= 1) *va_arg(ap, double *) = v;
                else *va_arg(ap, float *) = (float)v;
                assigned++;
            }
        } else {
            unsigned long long v = (conv == 'd' || conv == 'i') ? (unsigned long long)strtoll(tmp, &end, base)
                                                                 : strtoull(tmp, &end, base);
            if (end == tmp) return assigned;
            for (int i = n - 1; tmp + i >= end; i--) sunget(s, (unsigned char)tmp[i]);
            if (!suppress) {
                void *p = va_arg(ap, void *);
                if (conv == 'p') *(void **)p = (void *)(uintptr_t)v;
                else if (len <= -2) *(char *)p = (char)v;
                else if (len == -1) *(short *)p = (short)v;
                else if (len >= 2) *(long long *)p = (long long)v;
                else *(int *)p = (int)v;
                assigned++;
            }
        }
    }
    return assigned;
}

EXPORT int vsscanf(const char *str, const char *fmt, va_list ap) {
    StrCtx c = {str};
    Src s = {str_get, str_unget, &c, 0};
    return vscan(&s, fmt, ap);
}
EXPORT int sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
EXPORT int vfscanf(GFILE *f, const char *fmt, va_list ap) {
    Src s = {file_get, file_unget, f, 0};
    return vscan(&s, fmt, ap);
}
EXPORT int fscanf(GFILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

// ---- ctype (old bionic/OpenBSD tables; _ctype_ etc. are pointers) ----
#define _U 0x01
#define _L 0x02
#define _N 0x04
#define _S 0x08
#define _P 0x10
#define _C 0x20
#define _X 0x40
#define _B 0x80
static char ctype_tab[257];
static short tolower_tab[257], toupper_tab[257];
EXPORT const char *_ctype_ = ctype_tab;
EXPORT const short *_tolower_tab_ = tolower_tab;
EXPORT const short *_toupper_tab_ = toupper_tab;

__attribute__((constructor)) static void init_ctype(void) {
    tolower_tab[0] = toupper_tab[0] = -1;
    for (int c = 0; c < 256; c++) {
        char f = 0;
        if (c >= 'A' && c <= 'Z') f |= _U;
        if (c >= 'a' && c <= 'z') f |= _L;
        if (c >= '0' && c <= '9') f |= _N;
        if (c == ' ' || (c >= 9 && c <= 13)) f |= _S;
        if (c > 32 && c < 127 && !(f & (_U | _L | _N))) f |= _P;
        if (c < 32 || c == 127) f |= _C;
        if ((c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f')) f |= _X;
        if (c == ' ') f |= _B;
        ctype_tab[c + 1] = f;
        tolower_tab[c + 1] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        toupper_tab[c + 1] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
}

EXPORT int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
EXPORT int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
EXPORT int isspace(int c) { return isspace_(c); }
EXPORT int isdigit(int c) { return c >= '0' && c <= '9'; }
EXPORT int isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
EXPORT int isalnum(int c) { return isdigit(c) || isalpha(c); }
EXPORT int isupper(int c) { return c >= 'A' && c <= 'Z'; }
EXPORT int islower(int c) { return c >= 'a' && c <= 'z'; }
EXPORT int isxdigit(int c) { return isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
EXPORT int isprint(int c) { return c >= 32 && c < 127; }

// ---- misc data objects ----
EXPORT unsigned int __page_size = 4096;
EXPORT uintptr_t __stack_chk_guard = 0x5f3759df;
