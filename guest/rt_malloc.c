// SPDX-License-Identifier: GPL-3.0-or-later
// Guest heap: dlmalloc running inside the emulator, backed by the host mmap thunk.
#include <stdlib.h>
#include <malloc.h>
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define MMAP_CLEARS 1
#define USE_LOCKS 1
#define USE_SPIN_LOCKS 1
#define DEFAULT_GRANULARITY (4 * 1024 * 1024)
#define DEFAULT_MMAP_THRESHOLD (1024 * 1024)
#define malloc_getpagesize 4096
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 0
#define malloc_usable_size rt_malloc_usable_size
#define DLMALLOC_EXPORT __attribute__((visibility("default")))
#include "../third_party/misc/malloc.c"

DLMALLOC_EXPORT void *_Znwj(size_t n) { return malloc(n ? n : 1); }
DLMALLOC_EXPORT void *_Znaj(size_t n) { return malloc(n ? n : 1); }
DLMALLOC_EXPORT void _ZdlPv(void *p) { free(p); }
DLMALLOC_EXPORT void _ZdaPv(void *p) { free(p); }
