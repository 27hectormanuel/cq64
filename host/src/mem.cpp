// SPDX-License-Identifier: CC0-1.0
#include "common.h"

#include <sys/mman.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

// Host-side dlmalloc, used only as an mspace over guest memory.
#define ONLY_MSPACES 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 0
#define USE_LOCKS 1
#define MSPACES 1
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define FOOTERS 0
#define malloc_getpagesize 4096
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../../third_party/misc/malloc.c"
#pragma clang diagnostic pop

u8 *g_base;

void fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_write(ANDROID_LOG_FATAL, LOG_TAG, buf);
    abort();
}

namespace mem {
namespace {
std::mutex g_map_lock;
std::map<u32, u32> g_free;  // start -> size, page aligned, coalesced
mspace g_hheap;
std::mutex g_hheap_lock;
constexpr u32 PAGE = 4096;

void free_range(u32 a, u32 sz) {
    auto next = g_free.lower_bound(a);
    if (next != g_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == a) {
            a = prev->first;
            sz += prev->second;
            g_free.erase(prev);
        }
    }
    next = g_free.lower_bound(a);
    if (next != g_free.end() && a + sz == next->first) {
        sz += next->second;
        g_free.erase(next);
    }
    g_free[a] = sz;
}
}  // namespace

void init() {
    void *p = mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("cannot reserve 4GiB guest address space");
    g_base = (u8 *)p;
    // Keep the first 64 KiB inaccessible so guest null dereferences fault.
    mprotect(g_base, 0x10000, PROT_NONE);
    g_free[GUEST_MAP_BASE] = GUEST_MAP_END - GUEST_MAP_BASE;
    g_hheap = create_mspace_with_base(g_base + GUEST_HHEAP_BASE, GUEST_HHEAP_SIZE, 1);
    LOGI("guest address space at %p", g_base);
}

u32 map(u32 size, u32 hint) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (!size) return 0;
    std::lock_guard<std::mutex> lk(g_map_lock);
    if (hint) {
        // Honour the hint if the exact range is free.
        auto it = g_free.upper_bound(hint);
        if (it != g_free.begin()) {
            --it;
            if (it->first <= hint && hint + (u64)size <= (u64)it->first + it->second) {
                u32 s = it->first, sz = it->second;
                g_free.erase(it);
                if (hint > s) g_free[s] = hint - s;
                if (hint + size < s + sz) g_free[hint + size] = s + sz - (hint + size);
                return hint;
            }
        }
    }
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        if (it->second >= size) {
            u32 a = it->first, rest = it->second - size;
            g_free.erase(it);
            if (rest) g_free[a + size] = rest;
            return a;
        }
    }
    LOGE("guest address space exhausted (size %u)", size);
    return 0;
}

void unmap(u32 addr, u32 size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (!addr || !size) return;
    madvise(g_base + addr, size, MADV_DONTNEED);  // pages read back as zero
    std::lock_guard<std::mutex> lk(g_map_lock);
    free_range(addr, size);
}

u32 halloc(u32 size, u32 align) {
    std::lock_guard<std::mutex> lk(g_hheap_lock);
    void *p = mspace_memalign(g_hheap, align, size ? size : 1);
    if (!p) fatal("host heap exhausted");
    memset(p, 0, size);
    return h2g(p);
}

void hfree(u32 a) {
    if (!a) return;
    std::lock_guard<std::mutex> lk(g_hheap_lock);
    mspace_free(g_hheap, g2h(a));
}

u32 hstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    u32 a = halloc((u32)n, 1);
    memcpy(g2h(a), s, n);
    return a;
}
}  // namespace mem
