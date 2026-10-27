/* malloc family for the game libraries: newlib's heap first, then an overflow
 * mspace made from whatever user memory is still free once the game has
 * started its threads, mapped for the GPU like newlib's heap. Large requests
 * are logged with their caller so memory spikes can be traced to their
 * source. The eboot's own malloc family and newlib's reentrant one (stdio
 * buffers, FILEs, dtoa, strdup) are linked (--wrap) to the same functions, so
 * the port's code and libc can use the overflow too. */
#include <malloc.h>
#include <reent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "game/gamebin.h"
#include "util/log.h"

void *__real__malloc_r(struct _reent *r, size_t size);
void *__real__realloc_r(struct _reent *r, void *p, size_t size);
void *__real__memalign_r(struct _reent *r, size_t align, size_t size);
void __real__free_r(struct _reent *r, void *p);
void __malloc_lock(struct _reent *r);
void __malloc_unlock(struct _reent *r);

#define BIG (1024 * 1024)
#define TRACK (16 * 1024)

/* per-caller totals of tracked allocations (frees are not subtracted) */
static struct {
  uintptr_t lr;
  unsigned count;
  uint64_t bytes;
} callers[512];
static volatile int callers_lock;

extern so_module *gamebin_unity;

/* Unity funnels everything through its allocator; attribute to the first
 * libunity return address on the stack outside the allocator code. */
static void *unity_user(void *lr) {
  so_module *m = gamebin_unity;
  if (!m || (uintptr_t)lr != m->base + 0xDF5D4)
    return lr;
  uint32_t *sp = __builtin_frame_address(0);
  for (int i = 0; i < 96; i++) {
    uint32_t v = sp[i];
    if (v >= m->text_start && v < m->text_end) {
      uint32_t off = v - m->base;
      if ((off < 0xDB000 || off >= 0xE3000) && off < 0xE46000)
        return (void *)v;
    }
  }
  return lr;
}

static void track(size_t size, void *lr) {
  if (size < TRACK)
    return;
  lr = unity_user(lr);
  while (__atomic_exchange_n(&callers_lock, 1, __ATOMIC_ACQUIRE))
    ;
  for (int i = 0; i < 512; i++) {
    if (callers[i].lr == (uintptr_t)lr || !callers[i].lr) {
      callers[i].lr = (uintptr_t)lr;
      callers[i].count++;
      callers[i].bytes += size;
      break;
    }
  }
  __atomic_store_n(&callers_lock, 0, __ATOMIC_RELEASE);
}

void alloc_dump_callers(void) {
  extern int so_addr_info(uintptr_t, const char **, uintptr_t *, const char **, uintptr_t *);
  int used[512] = {0};
  for (int k = 0; k < 15; k++) {
    int best = -1;
    for (int i = 0; i < 512 && callers[i].lr; i++)
      if (!used[i] && (best < 0 || callers[i].bytes > callers[best].bytes))
        best = i;
    if (best < 0)
      break;
    used[best] = 1;
    const char *mod = "eboot", *sym;
    uintptr_t base = 0, sa;
    so_addr_info(callers[best].lr, &mod, &base, &sym, &sa);
    LOG("[alloc] top %2d: %s+0x%X %u allocs %u KB", k, mod, (unsigned)(callers[best].lr - base), callers[best].count,
        (unsigned)(callers[best].bytes / 1024));
  }
}

static void note(const char *fn, size_t size, void *p, void *lr) {
  const char *mod = "eboot", *sym;
  uintptr_t base = 0, sa;
  extern int so_addr_info(uintptr_t, const char **, uintptr_t *, const char **, uintptr_t *);
  so_addr_info((uintptr_t)lr, &mod, &base, &sym, &sa);
  LOG("[alloc] %s(%u KB) = %p from %s+0x%X", fn, (unsigned)(size / 1024), p, mod, (unsigned)((uintptr_t)lr - base));
}

/* Every heap refused; at most once a second, counting the ones in between */
static void oom(const char *fn, size_t size, void *lr) {
  static SceUInt64 last;
  static unsigned more;
  SceUInt64 now = sceKernelGetProcessTimeWide();
  if (now - last < 1000000) {
    more++;
    return;
  }
  last = now;
  const char *mod = "eboot", *sym;
  uintptr_t base = 0, sa;
  extern int so_addr_info(uintptr_t, const char **, uintptr_t *, const char **, uintptr_t *);
  so_addr_info((uintptr_t)lr, &mod, &base, &sym, &sa);
  LOG("[alloc] out of memory: %s(%u bytes) from %s+0x%X, %u more since the last report", fn, (unsigned)size, mod,
      (unsigned)((uintptr_t)lr - base), more);
  more = 0;
}

/* ---------- overflow heaps ---------- */

/* The kernel takes thread stacks and its own objects (semaphores…) from the
 * same memory: with all of it in heaps, loading the campaign failed to create
 * a semaphore while the heaps had 80 MB free. So the overflow heaps only take
 * it once the other heaps are full, and leave some behind. */
#define EXTRA_HEADROOM (4 * 1024 * 1024) /* left by the first overflow heap */
#define LAST_RESERVE (1024 * 1024)       /* still left once that is needed too */
#define PROBE (4 * 1024 * 1024)      /* how far past the reported free size to look */
#define PROBE_STEP (64 * 1024)
#define MAX_EXTRA 4

static struct {
  SceClibMspace space;
  uintptr_t lo, hi;
} extra[MAX_EXTRA];
static int nextra;
static SceKernelLwMutexWork extra_lock;

static int extra_of(const void *p) {
  for (int i = 0; i < nextra; i++)
    if ((uintptr_t)p >= extra[i].lo && (uintptr_t)p < extra[i].hi)
      return i;
  return -1;
}

static int in_extra(const void *p) {
  return extra_of(p) >= 0;
}

/* sceKernelGetFreeMemorySize reports ~2.5 MB less than the partition has
 * free (PSVshell's figure); the largest block the kernel will hand out, found
 * by bisection (every refused block is a kernel log message) */
static size_t user_free(int reported) {
  size_t lo = 0, hi = ((size_t)(reported > 0 ? reported : 0) + PROBE) & ~(PROBE_STEP - 1);
  while (hi - lo > PROBE_STEP) {
    size_t mid = lo + ((hi - lo) / 2 & ~(PROBE_STEP - 1));
    SceUID blk = sceKernelAllocMemBlock("pg3d_probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, mid, NULL);
    if (blk >= 0) {
      sceKernelFreeMemBlock(blk);
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* A new mspace from what the kernel has left, minus `keep` */
static int extra_add(size_t keep) {
  /* once refused, only look again after the kernel got memory back: out of
   * memory, every failed allocation lands here */
  static int refused_at = INT32_MIN;
  SceKernelFreeMemorySizeInfo fm = { .size = sizeof(fm) };
  sceKernelGetFreeMemorySize(&fm);
  int reported = (int)fm.size_user;
  if (reported <= refused_at)
    return -1;
  size_t avail = user_free(reported);
  if (nextra >= MAX_EXTRA || avail < keep + 256 * 1024) {
    refused_at = reported;
    return -1;
  }
  size_t size = (avail - keep) & ~0xfffu;
  SceUID blk = sceKernelAllocMemBlock("pg3d_heap2", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
  if (blk < 0) {
    LOG("[alloc] overflow heap of %u KB failed: 0x%08X", (unsigned)(size / 1024), blk);
    return -1;
  }
  void *base;
  sceKernelGetMemBlockBase(blk, &base);
  extra[nextra].lo = (uintptr_t)base;
  extra[nextra].hi = (uintptr_t)base + size;
  extra[nextra].space = sceClibMspaceCreate(base, size);
  /* vitaGL falls back to malloc once its pools are full, and the GPU reads
   * what it puts there: map this heap like vitaGL maps newlib's */
  int r = sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_RW);
  if (r < 0)
    LOG("[alloc] overflow heap %d: sceGxmMapMemory failed: 0x%08X", nextra + 1, r);
  nextra++;
  LOG("[alloc] overflow heap %d: %u KB at %p, %u KB left to the kernel", nextra, (unsigned)(size / 1024), base,
      (unsigned)((avail - size) / 1024));
  return 0;
}

static int extra_ready;

/* Overflow heaps from now on: Unity's worker threads have their stacks */
void alloc_extend(void) {
  if (extra_ready)
    return;
  sceKernelCreateLwMutex(&extra_lock, "pg3d_heap2", 0, 0, NULL);
  extra_ready = 1;
}

size_t alloc_extra_used(size_t *total) {
  size_t used = 0;
  *total = 0;
  if (!nextra)
    return 0;
  sceKernelLockLwMutex(&extra_lock, 1, NULL);
  for (int i = 0; i < nextra; i++) {
    SceClibMspaceStats st;
    sceClibMspaceMallocStatsFast(extra[i].space, &st);
    used += st.current_in_use;
    *total += extra[i].hi - extra[i].lo;
  }
  sceKernelUnlockLwMutex(&extra_lock, 1);
  return used;
}

static void *extra_memalign(size_t align, size_t size) {
  if (!nextra)
    return NULL;
  void *p = NULL;
  sceKernelLockLwMutex(&extra_lock, 1, NULL);
  for (int i = 0; i < nextra && !p; i++)
    p = sceClibMspaceMemalign(extra[i].space, align < 8 ? 8 : align, size ? size : 1);
  sceKernelUnlockLwMutex(&extra_lock, 1);
  return p;
}

/* Out of every heap: a new one from the kernel's memory; past the first,
 * the headroom goes too, bar one stack */
static void *extra_grow(size_t align, size_t size) {
  if (!extra_ready)
    return NULL;
  void *p = NULL;
  sceKernelLockLwMutex(&extra_lock, 1, NULL);
  if (extra_add(nextra ? LAST_RESERVE : EXTRA_HEADROOM) == 0)
    p = sceClibMspaceMemalign(extra[nextra - 1].space, align < 8 ? 8 : align, size ? size : 1);
  sceKernelUnlockLwMutex(&extra_lock, 1);
  return p;
}

/* Caches that give memory back when both heaps are out (game/zipcache.c) */
static size_t (*reclaim)(size_t want);

void alloc_set_reclaim(size_t (*fn)(size_t want)) {
  reclaim = fn;
}

static int reclaimed(size_t size) {
  return reclaim && reclaim(size) > 0;
}

/* For thread stacks, which the kernel takes from what the heaps left */
int alloc_reclaim(size_t want) {
  return reclaimed(want);
}

/* ---------- the big heap ---------- */

/* Requests from BIG_MIN up get a heap of their own, carved out of newlib's at
 * startup. In one shared heap small allocations settle in the holes big frees
 * leave: after a match newlib's had 55 MB free in pieces too small for one
 * 4 MB block, and Unity gave up loading the campaign. Either side falls back
 * to the other. */
#define BIG_MIN (256 * 1024)

static struct {
  SceClibMspace space;
  uintptr_t lo, hi;
} big;
static SceKernelLwMutexWork big_lock;

static int in_big(const void *p) {
  return (uintptr_t)p >= big.lo && (uintptr_t)p < big.hi;
}

/* At startup, before the game's memory is spoken for */
void alloc_init_big(size_t size) {
  SceUID blk = sceKernelAllocMemBlock("pg3d_big", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
  if (blk < 0) {
    LOG("[alloc] big heap of %u KB failed: 0x%08X", (unsigned)(size / 1024), blk);
    return;
  }
  void *base;
  sceKernelGetMemBlockBase(blk, &base);
  sceKernelCreateLwMutex(&big_lock, "pg3d_big", 0, 0, NULL);
  big.lo = (uintptr_t)base;
  big.hi = (uintptr_t)base + size;
  big.space = sceClibMspaceCreate(base, size);
  LOG("[alloc] big heap: %u KB at %p for requests from %u KB", (unsigned)(size / 1024), base, BIG_MIN / 1024);
}

/* Once sceGxm is up: vitaGL puts textures in malloc memory when VRAM is full */
void alloc_map_big(void) {
  if (!big.space)
    return;
  int r = sceGxmMapMemory((void *)big.lo, big.hi - big.lo, SCE_GXM_MEMORY_ATTRIB_RW);
  if (r < 0)
    LOG("[alloc] big heap: sceGxmMapMemory failed: 0x%08X", r);
}

size_t alloc_big_used(size_t *peak, size_t *total) {
  *peak = 0;
  *total = big.hi - big.lo;
  if (!big.space)
    return 0;
  SceClibMspaceStats st;
  sceKernelLockLwMutex(&big_lock, 1, NULL);
  sceClibMspaceMallocStatsFast(big.space, &st);
  sceKernelUnlockLwMutex(&big_lock, 1);
  *peak = st.peak_in_use;
  return st.current_in_use;
}

static void *big_memalign(size_t align, size_t size) {
  if (!big.space)
    return NULL;
  sceKernelLockLwMutex(&big_lock, 1, NULL);
  void *p = sceClibMspaceMemalign(big.space, align < 8 ? 8 : align, size ? size : 1);
  sceKernelUnlockLwMutex(&big_lock, 1);
  return p;
}

/* ---------- newlib's heap ---------- */

/* newlib's realloc and memalign build on a _malloc_r block of their own and
 * treat it as one of newlib's chunks, so while a thread is inside them its
 * _malloc_r calls stay in newlib's heap (and the fallbacks, which take other
 * locks, never run under newlib's) */
static volatile SceUID newlib_only = -1;

static void *heap_malloc(size_t size) {
  return __real__malloc_r(_REENT, size);
}

static void *heap_realloc(void *p, size_t size) {
  __malloc_lock(_REENT);
  newlib_only = sceKernelGetThreadId();
  p = __real__realloc_r(_REENT, p, size);
  newlib_only = -1;
  __malloc_unlock(_REENT);
  return p;
}

static void *heap_memalign(size_t align, size_t size) {
  if (align <= 8)
    return heap_malloc(size);
  __malloc_lock(_REENT);
  newlib_only = sceKernelGetThreadId();
  void *p = __real__memalign_r(_REENT, align, size);
  newlib_only = -1;
  __malloc_unlock(_REENT);
  return p;
}

/* Every heap failed: empty the caches and retry, then grow */
static void *last_resort(size_t align, size_t size) {
  void *p = NULL;
  if (reclaimed(size) && !(p = heap_memalign(align, size)) && !(p = extra_memalign(align, size)))
    p = big_memalign(align, size);
  return p ? p : extra_grow(align, size);
}

/* Every heap, in the order that suits the size */
static void *any_memalign(size_t align, size_t size) {
  void *p = size >= BIG_MIN ? big_memalign(align, size) : NULL;
  if (!p)
    p = heap_memalign(align, size);
  if (!p)
    p = extra_memalign(align, size);
  if (!p && size < BIG_MIN)
    p = big_memalign(align, size);
  return p ? p : last_resort(align, size);
}

void b_free(void *p) {
  int i = extra_of(p);
  if (i >= 0) {
    sceKernelLockLwMutex(&extra_lock, 1, NULL);
    sceClibMspaceFree(extra[i].space, p);
    sceKernelUnlockLwMutex(&extra_lock, 1);
  } else if (in_big(p)) {
    sceKernelLockLwMutex(&big_lock, 1, NULL);
    sceClibMspaceFree(big.space, p);
    sceKernelUnlockLwMutex(&big_lock, 1);
  } else {
    __real__free_r(_REENT, p);
  }
}

static size_t usable(void *p) {
  return in_extra(p) || in_big(p) ? sceClibMspaceMallocUsableSize(p) : malloc_usable_size(p);
}

static void *do_malloc(size_t size, void *lr) {
  void *p = any_memalign(8, size);
  if (!p && size)
    oom("malloc", size, lr);
  track(size, lr);
  if (size >= BIG)
    note("malloc", size, p, lr);
  return p;
}

static void *do_calloc(size_t n, size_t size, void *lr) {
  if (size && n > (size_t)-1 / size)
    return NULL;
  size *= n;
  /* not newlib's calloc: it sizes the memset from the chunk header of
   * whatever _malloc_r returned, which may be an overflow block */
  void *p = any_memalign(8, size);
  if (p)
    memset(p, 0, size);
  if (!p && size)
    oom("calloc", size, lr);
  track(size, lr);
  if (size >= BIG)
    note("calloc", size, p, lr);
  return p;
}

static void *do_realloc(void *old, size_t size, void *lr) {
  void *p = NULL;
  int i = extra_of(old);
  if (!old) {
    p = any_memalign(8, size);
  } else if (i >= 0) {
    sceKernelLockLwMutex(&extra_lock, 1, NULL);
    p = sceClibMspaceRealloc(extra[i].space, old, size);
    sceKernelUnlockLwMutex(&extra_lock, 1);
  } else if (in_big(old)) {
    sceKernelLockLwMutex(&big_lock, 1, NULL);
    p = sceClibMspaceRealloc(big.space, old, size);
    sceKernelUnlockLwMutex(&big_lock, 1);
  } else if (size < BIG_MIN || !big.space) {
    p = heap_realloc(old, size);
  } /* else grown past BIG_MIN: moves to the big heap */
  if (!p && size && old) {
    /* move to another heap */
    p = any_memalign(8, size);
    if (p) {
      size_t n = usable(old);
      memcpy(p, old, n < size ? n : size);
      b_free(old);
    }
  }
  if (!p && size)
    oom("realloc", size, lr);
  track(size, lr);
  if (size >= BIG)
    note("realloc", size, p, lr);
  return p;
}

static void *do_memalign(size_t align, size_t size, void *lr) {
  void *p = any_memalign(align, size);
  if (!p && size)
    oom("memalign", size, lr);
  track(size, lr);
  if (size >= BIG)
    note("memalign", size, p, lr);
  return p;
}

void *b_malloc(size_t size) {
  return do_malloc(size, __builtin_return_address(0));
}

void *b_calloc(size_t n, size_t size) {
  return do_calloc(n, size, __builtin_return_address(0));
}

void *b_realloc(void *old, size_t size) {
  return do_realloc(old, size, __builtin_return_address(0));
}

void *b_memalign(size_t align, size_t size) {
  return do_memalign(align, size, __builtin_return_address(0));
}

void *__wrap_malloc(size_t size) __attribute__((alias("b_malloc")));
void *__wrap_calloc(size_t n, size_t size) __attribute__((alias("b_calloc")));
void *__wrap_realloc(void *p, size_t size) __attribute__((alias("b_realloc")));
void *__wrap_memalign(size_t align, size_t size) __attribute__((alias("b_memalign")));
void __wrap_free(void *p) __attribute__((alias("b_free")));

/* libc's own allocations; a failed stdio buffer leaves a FILE unbuffered,
 * one sceIoWrite per byte (a 425 KB prefs save took ~40 minutes) */
void *__wrap__malloc_r(struct _reent *r, size_t size) {
  if (newlib_only == sceKernelGetThreadId())
    return __real__malloc_r(r, size);
  return do_malloc(size, __builtin_return_address(0));
}

void *__wrap__calloc_r(struct _reent *r, size_t n, size_t size) {
  return do_calloc(n, size, __builtin_return_address(0));
}

void *__wrap__realloc_r(struct _reent *r, void *old, size_t size) {
  return do_realloc(old, size, __builtin_return_address(0));
}

void *__wrap__memalign_r(struct _reent *r, size_t align, size_t size) {
  return do_memalign(align, size, __builtin_return_address(0));
}

void __wrap__free_r(struct _reent *r, void *p) {
  b_free(p);
}
