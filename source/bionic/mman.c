/*
 * mmap/munmap/mprotect over one memory block reserved at startup (before
 * VitaGL takes the remaining RAM). Page allocation uses its own lock, never
 * newlib malloc, so the GC can map memory while other threads are suspended.
 * Once the block is full, whole pages come from the malloc heaps instead,
 * but only while no thread is suspended (one could hold a heap lock).
 */
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bionic.h"
#include "util/log.h"

#define PAGE 4096
#define B_MAP_FIXED 0x10
#define B_MAP_ANON 0x20
#define B_PROT_WRITE 2
#define MAP_FAILED_ ((void *)-1)

static uintptr_t arena_base;
static size_t arena_pages;
static uint32_t *bitmap; /* 1 = used */
static size_t hint;
static SceKernelLwMutexWork lock;
static size_t used_pages, peak_pages;

#define MAX_CHUNKS 512
static struct {
  uintptr_t base;
  size_t len;
} chunks[MAX_CHUNKS]; /* page runs taken from the malloc heaps */
static int n_chunks;
static size_t chunk_bytes, chunk_peak;

void *b_memalign(size_t align, size_t size);
void b_free(void *p);
int gc_world_stopped(void);

static inline int page_used(size_t i) { return (bitmap[i >> 5] >> (i & 31)) & 1; }
static inline void page_set(size_t i, int v) {
  if (v)
    bitmap[i >> 5] |= 1u << (i & 31);
  else
    bitmap[i >> 5] &= ~(1u << (i & 31));
}

int mman_init(size_t size) {
  size = (size + 0xfffff) & ~0xfffff;
  SceUID blk = sceKernelAllocMemBlock("pg3d_mmap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
  if (blk < 0) {
    LOG("[mman] reserve 0x%X failed: 0x%08X", size, blk);
    return blk;
  }
  void *p;
  sceKernelGetMemBlockBase(blk, &p);
  arena_base = (uintptr_t)p;
  arena_pages = size / PAGE;
  /* bitmap lives in its own block too */
  size_t bm = ((arena_pages + 31) / 32 * 4 + 0xfff) & ~0xfff;
  SceUID bblk = sceKernelAllocMemBlock("pg3d_mmap_bm", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, bm, NULL);
  if (bblk < 0)
    return bblk;
  sceKernelGetMemBlockBase(bblk, (void **)&bitmap);
  sceClibMemset(bitmap, 0, bm);
  sceKernelCreateLwMutex(&lock, "pg3d_mmap", 0, 0, NULL);
  LOG("[mman] arena %p size %u MB", p, (unsigned)(size >> 20));
  return 0;
}

static uintptr_t alloc_pages(size_t n) {
  for (int pass = 0; pass < 2; pass++) {
    size_t start = pass ? 0 : hint;
    size_t run = 0;
    for (size_t i = start; i < arena_pages; i++) {
      if (page_used(i)) {
        run = 0;
        continue;
      }
      if (++run == n) {
        size_t first = i + 1 - n;
        for (size_t k = first; k <= i; k++)
          page_set(k, 1);
        hint = i + 1;
        used_pages += n;
        if (used_pages > peak_pages)
          peak_pages = used_pages;
        return arena_base + first * PAGE;
      }
    }
  }
  return 0;
}

static int in_arena(uintptr_t a, size_t len) {
  return a >= arena_base && a + len <= arena_base + arena_pages * PAGE;
}

/* index of the chunk holding [a, a + len), or -1; call with lock held */
static int find_chunk(uintptr_t a, size_t len) {
  for (int i = 0; i < n_chunks; i++)
    if (a >= chunks[i].base && a + len <= chunks[i].base + chunks[i].len)
      return i;
  return -1;
}

static uintptr_t alloc_chunk(size_t len) {
  if (gc_world_stopped())
    return 0;
  void *p = b_memalign(PAGE, len);
  if (!p)
    return 0;
  sceKernelLockLwMutex(&lock, 1, NULL);
  int ok = n_chunks < MAX_CHUNKS;
  if (ok) {
    chunks[n_chunks].base = (uintptr_t)p;
    chunks[n_chunks++].len = len;
    chunk_bytes += len;
    if (chunk_bytes > chunk_peak)
      chunk_peak = chunk_bytes;
  }
  sceKernelUnlockLwMutex(&lock, 1);
  if (!ok) {
    b_free(p);
    return 0;
  }
  LOG("[mman] arena full: %u KB from the heap (%d chunks, %u KB)", (unsigned)(len / 1024), n_chunks,
      (unsigned)(chunk_bytes / 1024));
  return (uintptr_t)p;
}

void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  if (!len)
    return MAP_FAILED_;
  size_t n = (len + PAGE - 1) / PAGE;
  uintptr_t a;

  if (flags & B_MAP_FIXED) {
    sceKernelLockLwMutex(&lock, 1, NULL);
    int mine = in_arena((uintptr_t)addr, n * PAGE) || find_chunk((uintptr_t)addr, n * PAGE) >= 0;
    sceKernelUnlockLwMutex(&lock, 1);
    if (!mine) {
      LOG("[mman] MAP_FIXED outside arena %p len 0x%X", addr, len);
      bionic_set_errno(22);
      return MAP_FAILED_;
    }
    /* GC_unmap/GC_remap: remapping inside our arena. Keep pages reserved,
     * hand back zeroed memory when it becomes accessible again. */
    a = (uintptr_t)addr;
    if (prot & B_PROT_WRITE)
      sceClibMemset((void *)a, 0, n * PAGE);
    return (void *)a;
  }

  sceKernelLockLwMutex(&lock, 1, NULL);
  a = alloc_pages(n);
  sceKernelUnlockLwMutex(&lock, 1);
  if (!a)
    a = alloc_chunk(n * PAGE);
  if (!a) {
    LOG("[mman] out of memory: want %u pages, arena %u/%u used, %u KB from the heap", (unsigned)n,
        (unsigned)used_pages, (unsigned)arena_pages, (unsigned)(chunk_bytes / 1024));
    bionic_set_errno(12);
    return MAP_FAILED_;
  }
  sceClibMemset((void *)a, 0, n * PAGE);

  if (!(flags & B_MAP_ANON) && fd >= 0) {
    extern long b_lseek(int, long, int);
    extern long b_read(int, void *, size_t);
    long cur = b_lseek(fd, 0, 1);
    b_lseek(fd, off, 0);
    size_t done = 0;
    while (done < len) {
      long r = b_read(fd, (void *)(a + done), len - done);
      if (r <= 0)
        break;
      done += r;
    }
    b_lseek(fd, cur, 0);
    DLOG("[mman] file mmap fd %d off 0x%lX len 0x%X -> %p (%u read)", fd, off, len, (void *)a, (unsigned)done);
  }
  return (void *)a;
}

int b_munmap(void *addr, size_t len) {
  uintptr_t a = (uintptr_t)addr;
  size_t n = (len + PAGE - 1) / PAGE;
  sceKernelLockLwMutex(&lock, 1, NULL);
  if (!in_arena(a, n * PAGE)) {
    int i = find_chunk(a, n * PAGE);
    void *whole = NULL;
    /* a partial unmap keeps the chunk; the GC only reuses it through MAP_FIXED */
    if (i >= 0 && a == chunks[i].base && n * PAGE >= chunks[i].len) {
      whole = (void *)chunks[i].base;
      chunk_bytes -= chunks[i].len;
      chunks[i] = chunks[--n_chunks];
    }
    sceKernelUnlockLwMutex(&lock, 1);
    if (whole)
      b_free(whole);
    else if (i < 0)
      LOG("[mman] munmap outside arena %p", addr);
    return 0;
  }
  size_t first = (a - arena_base) / PAGE;
  for (size_t k = first; k < first + n; k++) {
    if (page_used(k)) {
      page_set(k, 0);
      used_pages--;
    }
  }
  if (first < hint)
    hint = first;
  sceKernelUnlockLwMutex(&lock, 1);
  return 0;
}

int b_mprotect(void *addr, size_t len, int prot) {
  return 0;
}

void mman_stats(size_t *used, size_t *peak, size_t *total) {
  *used = used_pages * PAGE + chunk_bytes;
  *peak = peak_pages * PAGE + chunk_peak;
  *total = arena_pages * PAGE + chunk_bytes;
}
