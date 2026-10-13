// SPDX-License-Identifier: CC0-1.0
// Guest-side (ARM32) implementations of pure libc functions.
// Running these inside the emulator avoids a JIT exit per call.
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>

#define EXPORT __attribute__((visibility("default")))

extern int *__errno(void);

EXPORT void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
        while (n >= 16) {
            uint32_t a = ((const uint32_t *)sp)[0], b = ((const uint32_t *)sp)[1];
            uint32_t c = ((const uint32_t *)sp)[2], e = ((const uint32_t *)sp)[3];
            ((uint32_t *)dp)[0] = a; ((uint32_t *)dp)[1] = b;
            ((uint32_t *)dp)[2] = c; ((uint32_t *)dp)[3] = e;
            dp += 16; sp += 16; n -= 16;
        }
        while (n >= 4) { *(uint32_t *)dp = *(const uint32_t *)sp; dp += 4; sp += 4; n -= 4; }
    }
    while (n--) *dp++ = *sp++;
    return d;
}

EXPORT void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n; sp += n;
    while (n--) *--dp = *--sp;
    return d;
}

EXPORT void *memset(void *d, int c, size_t n) {
    unsigned char *dp = d;
    uint32_t v = (unsigned char)c;
    v |= v << 8; v |= v << 16;
    while (n && ((uintptr_t)dp & 3)) { *dp++ = (unsigned char)c; n--; }
    while (n >= 16) {
        ((uint32_t *)dp)[0] = v; ((uint32_t *)dp)[1] = v;
        ((uint32_t *)dp)[2] = v; ((uint32_t *)dp)[3] = v;
        dp += 16; n -= 16;
    }
    while (n >= 4) { *(uint32_t *)dp = v; dp += 4; n -= 4; }
    while (n--) *dp++ = (unsigned char)c;
    return d;
}

EXPORT int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

EXPORT void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c) return (void *)p;
    return 0;
}

EXPORT void *memmem(const void *h, size_t hl, const void *n, size_t nl) {
    const unsigned char *hp = h;
    if (nl == 0) return (void *)h;
    if (hl < nl) return 0;
    for (size_t i = 0; i + nl <= hl; i++)
        if (hp[i] == *(const unsigned char *)n && memcmp(hp + i, n, nl) == 0) return (void *)(hp + i);
    return 0;
}

EXPORT size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return p - s;
}

EXPORT int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

EXPORT int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

EXPORT int strcoll(const char *a, const char *b) { return strcmp(a, b); }

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

EXPORT int strcasecmp(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

EXPORT int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

EXPORT char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

EXPORT char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    for (; n && *s; n--) *d++ = *s++;
    for (; n; n--) *d++ = 0;
    return r;
}

EXPORT char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}

EXPORT char *strncat(char *d, const char *s, size_t n) {
    char *p = d + strlen(d);
    for (; n && *s; n--) *p++ = *s++;
    *p = 0;
    return d;
}

EXPORT size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}

EXPORT size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = 0;
    while (dl < n && d[dl]) dl++;
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}

EXPORT char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

EXPORT char *strrchr(const char *s, int c) {
    const char *r = 0;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

EXPORT char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++)
        if (*h == *n && strncmp(h, n, nl) == 0) return (char *)h;
    return 0;
}

EXPORT size_t strspn(const char *s, const char *a) {
    size_t i = 0;
    while (s[i] && strchr(a, s[i])) i++;
    return i;
}

EXPORT size_t strcspn(const char *s, const char *r) {
    size_t i = 0;
    while (s[i] && !strchr(r, s[i])) i++;
    return i;
}

EXPORT char *strpbrk(const char *s, const char *a) {
    s += strcspn(s, a);
    return *s ? (char *)s : 0;
}

EXPORT char *strtok(char *s, const char *sep) {
    static char *save;
    if (!s) s = save;
    if (!s) return 0;
    s += strspn(s, sep);
    if (!*s) { save = 0; return 0; }
    char *e = s + strcspn(s, sep);
    if (*e) { *e = 0; save = e + 1; } else save = 0;
    return s;
}

extern void *malloc(size_t);
EXPORT char *strdup(const char *s) {
    size_t l = strlen(s) + 1;
    char *d = malloc(l);
    if (d) memcpy(d, s, l);
    return d;
}

EXPORT size_t wcslen(const wchar_t *s) {
    const wchar_t *p = s;
    while (*p) p++;
    return p - s;
}

EXPORT wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n) {
    wchar_t *r = d;
    for (; n && *s; n--) *d++ = *s++;
    for (; n; n--) *d++ = 0;
    return r;
}

EXPORT int iswspace(unsigned c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xa0 || c == 0x3000; }
EXPORT int iswalnum(unsigned c) {
    if (c < 128) return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
    return !iswspace(c);
}

// ---- number parsing ----
static int isspace_(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int digitval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    return 99;
}

static unsigned long long parse_ull(const char *s, char **end, int base, int *neg, int *overflow) {
    const char *p = s;
    unsigned long long v = 0;
    int any = 0;
    *neg = 0; *overflow = 0;
    while (isspace_(*p)) p++;
    if (*p == '-') { *neg = 1; p++; } else if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && digitval(p[2]) < 16) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    for (;; p++) {
        int d = digitval(*p);
        if (d >= base) break;
        unsigned long long nv = v * base + d;
        if (nv / base != v && !*overflow) *overflow = 1;
        v = nv;
        any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    return v;
}

EXPORT long long strtoll(const char *s, char **end, int base) {
    int neg, of;
    unsigned long long v = parse_ull(s, end, base, &neg, &of);
    if (of || (!neg && v > LLONG_MAX) || (neg && v > (unsigned long long)LLONG_MAX + 1)) {
        *__errno() = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? -(long long)v : (long long)v;
}

EXPORT unsigned long long strtoull(const char *s, char **end, int base) {
    int neg, of;
    unsigned long long v = parse_ull(s, end, base, &neg, &of);
    if (of) { *__errno() = ERANGE; return ULLONG_MAX; }
    return neg ? -v : v;
}

EXPORT long strtol(const char *s, char **end, int base) {
    int neg, of;
    unsigned long long v = parse_ull(s, end, base, &neg, &of);
    if (of || (!neg && v > LONG_MAX) || (neg && v > (unsigned long long)LONG_MAX + 1)) {
        *__errno() = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? -(long)v : (long)v;
}

EXPORT unsigned long strtoul(const char *s, char **end, int base) {
    int neg, of;
    unsigned long long v = parse_ull(s, end, base, &neg, &of);
    if (of || v > ULONG_MAX) { *__errno() = ERANGE; return ULONG_MAX; }
    return neg ? -(unsigned long)v : (unsigned long)v;
}

EXPORT int atoi(const char *s) { return (int)strtol(s, 0, 10); }
EXPORT long atol(const char *s) { return strtol(s, 0, 10); }

static const double pow10tab[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
                                  1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
static double scale10(double v, int e) {
    int neg = e < 0;
    if (neg) e = -e;
    double m = 1.0, b = 10.0;
    while (e) {
        if (e & 1) m *= b;
        b *= b;
        e >>= 1;
    }
    return neg ? v / m : v * m;
}

EXPORT double strtod(const char *s, char **end) {
    const char *p = s;
    int neg = 0, any = 0, exp10 = 0;
    unsigned long long mant = 0;
    int mdigits = 0;
    while (isspace_(*p)) p++;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') {
        p += 3;
        if (end) *end = (char *)p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') {
        p += 3;
        if (end) *end = (char *)p;
        return __builtin_nan("");
    }
    for (; *p >= '0' && *p <= '9'; p++) {
        any = 1;
        if (mdigits < 19) { mant = mant * 10 + (*p - '0'); if (mant) mdigits++; }
        else exp10++;
    }
    if (*p == '.') {
        p++;
        for (; *p >= '0' && *p <= '9'; p++) {
            any = 1;
            if (mdigits < 19) { mant = mant * 10 + (*p - '0'); if (mant) mdigits++; exp10--; }
        }
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0.0;
    }
    if ((*p | 32) == 'e') {
        const char *q = p + 1;
        int eneg = 0, e = 0, edig = 0;
        if (*q == '-') { eneg = 1; q++; } else if (*q == '+') q++;
        for (; *q >= '0' && *q <= '9'; q++) { if (e < 10000) e = e * 10 + (*q - '0'); edig = 1; }
        if (edig) { p = q; exp10 += eneg ? -e : e; }
    }
    if (end) *end = (char *)p;
    double v = (double)mant;
    if (exp10 >= -22 && exp10 <= 22) v = exp10 < 0 ? v / pow10tab[-exp10] : v * pow10tab[exp10];
    else v = scale10(v, exp10);
    return neg ? -v : v;
}

EXPORT float strtof(const char *s, char **end) { return (float)strtod(s, end); }
EXPORT double atof(const char *s) { return strtod(s, 0); }

EXPORT int abs(int x) { return x < 0 ? -x : x; }
EXPORT long labs(long x) { return x < 0 ? -x : x; }

// ---- rand48 ----
static unsigned long long rand48_state = 0x1234ABCD330EULL;
EXPORT void srand48(long seed) { rand48_state = ((unsigned long long)(unsigned long)seed << 16) | 0x330E; }
EXPORT long lrand48(void) {
    rand48_state = (rand48_state * 0x5DEECE66DULL + 0xB) & 0xFFFFFFFFFFFFULL;
    return (long)(rand48_state >> 17);
}
static unsigned rand_state = 1;
EXPORT void srand(unsigned s) { rand_state = s; }
EXPORT int rand(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (rand_state >> 1) & 0x7fffffff;
}

// ---- sorting ----
static void swapb(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

EXPORT void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    char *b = base;
    // Iterative quicksort with insertion sort for small partitions.
    struct { size_t lo, hi; } stack[64];
    int sp = 0;
    if (n < 2) return;
    stack[sp].lo = 0; stack[sp].hi = n - 1; sp++;
    while (sp) {
        sp--;
        size_t lo = stack[sp].lo, hi = stack[sp].hi;
        while (hi > lo) {
            if (hi - lo < 12) {
                for (size_t i = lo + 1; i <= hi; i++)
                    for (size_t j = i; j > lo && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--)
                        swapb(b + (j - 1) * sz, b + j * sz, sz);
                break;
            }
            size_t mid = lo + (hi - lo) / 2;
            if (cmp(b + mid * sz, b + lo * sz) < 0) swapb(b + mid * sz, b + lo * sz, sz);
            if (cmp(b + hi * sz, b + lo * sz) < 0) swapb(b + hi * sz, b + lo * sz, sz);
            if (cmp(b + hi * sz, b + mid * sz) < 0) swapb(b + hi * sz, b + mid * sz, sz);
            swapb(b + mid * sz, b + hi * sz, sz);  // pivot at hi
            char *pv = b + hi * sz;
            size_t i = lo;
            for (size_t j = lo; j < hi; j++)
                if (cmp(b + j * sz, pv) < 0) { swapb(b + i * sz, b + j * sz, sz); i++; }
            swapb(b + i * sz, pv, sz);
            // push larger side, loop on smaller
            if (i - lo > hi - i) {
                if (i > lo + 1) { stack[sp].lo = lo; stack[sp].hi = i - 1; sp++; }
                lo = i + 1;
            } else {
                if (hi > i + 1) { stack[sp].lo = i + 1; stack[sp].hi = hi; sp++; }
                if (i == 0) break;
                hi = i - 1;
            }
        }
    }
}

EXPORT void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    const char *b = base;
    while (n) {
        const char *m = b + (n / 2) * sz;
        int c = cmp(key, m);
        if (c == 0) return (void *)m;
        if (c > 0) { b = m + sz; n -= n / 2 + 1; }
        else n /= 2;
    }
    return 0;
}

// ---- old libgcc helper ----
EXPORT long long __gnu_ldivmod_helper(long long a, long long b, long long *rem) {
    long long q = a / b;
    *rem = a - q * b;
    return q;
}
EXPORT unsigned long long __gnu_uldivmod_helper(unsigned long long a, unsigned long long b, unsigned long long *rem) {
    unsigned long long q = a / b;
    *rem = a - q * b;
    return q;
}
EXPORT int __aeabi_idiv0(int r) { return r; }
EXPORT long long __aeabi_ldiv0(long long r) { return r; }
