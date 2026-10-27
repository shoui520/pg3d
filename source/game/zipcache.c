/*
 * Unity reads APK/OBB entries through a stream that inflates as it goes.
 * Seeking backwards restarts the inflate at the entry's start and skips
 * forward 1 KB at a time, and the serialized-file reader seeks around inside
 * entries constantly: entering the arena decompressed one 680 KB entry 1400
 * times over (30 s). An entry a stream seeks back in, or that a second stream
 * opens, is decompressed once into an LRU cache instead, and its streams are
 * served from there.
 *
 * A stream served from the cache keeps its position in z.total_out (what
 * Unity's Tell returns for these streams) and is marked through z.reserved,
 * which zlib never touches and Unity zeroes. If its entry can't be cached it
 * goes back to Unity's code through Unity's own restart path.
 *
 * The cache has a memory block of its own, so its churn never fragments the
 * heaps Unity allocates from. The first time an allocation fails anyway, the
 * cache is switched off for the session and the block goes back to the
 * kernel, where the allocator can grow into it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/archive.h"
#include "game/gamebin.h"
#include "util/log.h"
#include "util/perf.h"

#define BUDGET (8 * 1024 * 1024)       /* cached data */
#define REGION (BUDGET + 256 * 1024)    /* + entries, zlib state, input chunk */
#define CHUNK (64 * 1024)               /* compressed input per read */
#define ENTRY_MAX (2 * 1024 * 1024)
#define SERVED 0x5A43u
#define SEEN_SLOTS 16384

/* libunity's zip entry stream */
typedef struct {
  void **vtable;
  void *file;            /* the archive */
  const uint8_t *header; /* the entry's 30-byte local header */
  uint32_t start;        /* where its data starts in the archive */
  uint32_t pos;          /* compressed bytes consumed */
  int buf;
  z_stream z;
} zip_stream;
_Static_assert(__builtin_offsetof(zip_stream, z) == 24, "zip stream layout");
_Static_assert(__builtin_offsetof(zip_stream, z.reserved) == 76, "z_stream layout");

typedef struct entry {
  struct entry *prev, *next; /* most recently used first */
  uint32_t start, crc, csize, size;
  uint8_t *data;
} entry;

typedef int (*read_fn)(zip_stream *s, void *dst, int n);
typedef int (*seek_fn)(zip_stream *s, int off, int whence);
static read_fn unity_read;
static seek_fn unity_seek;

static SceKernelLwMutexWork lock; /* everything below; held through a load */
static SceUID block = -1;
static SceClibMspace space; /* NULL once given back */
static uint8_t *chunk;
static entry *lru;
static uint32_t cached_bytes;
static uint32_t seen[SEEN_SLOTS]; /* data offsets of entries a stream has started on */

static uint32_t rd32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static int deflated(const zip_stream *s) {
  return s->header[8] == 8 && s->header[9] == 0;
}

static uint32_t entry_size(const zip_stream *s) {
  return rd32(s->header + 22);
}

/* under the lock */
static entry *find(const zip_stream *s) {
  uint32_t crc = rd32(s->header + 14), csize = rd32(s->header + 18);
  for (entry *e = lru; e; e = e->next) {
    if (e->start != s->start || e->crc != crc || e->csize != csize)
      continue;
    if (e != lru) {
      e->prev->next = e->next;
      if (e->next)
        e->next->prev = e->prev;
      e->prev = NULL;
      e->next = lru;
      lru->prev = e;
      lru = e;
    }
    return e;
  }
  return NULL;
}

/* under the lock: drops the least recently used entry */
static int evict_one(void) {
  entry *last = lru;
  while (last && last->next)
    last = last->next;
  if (!last)
    return 0;
  if (last->prev)
    last->prev->next = NULL;
  else
    lru = NULL;
  cached_bytes -= last->size;
  sceClibMspaceFree(space, last->data);
  sceClibMspaceFree(space, last);
  return 1;
}

/* under the lock: from the cache's block, evicting until it fits */
static void *region_alloc(uint32_t size) {
  void *p;
  while (!(p = sceClibMspaceMalloc(space, size)) && evict_one())
    ;
  return p;
}

static voidpf zalloc(voidpf opaque, uInt items, uInt size) {
  return region_alloc(items * size);
}

static void zfree(voidpf opaque, voidpf p) {
  sceClibMspaceFree(space, p);
}

/* The whole entry from one archive, checked against its CRC. Unity's copy of
 * the local header has the sizes but, for entries written with a data
 * descriptor (flag bit 3, every deflated OBB entry), no CRC: that one is in
 * the descriptor after the data. Under the lock. */
static const char *inflate_entry(const zip_stream *s, int archive, uint8_t *out, uint32_t size) {
  uint32_t csize = rd32(s->header + 18), crc = rd32(s->header + 14), done = 0;
  z_stream z;
  memset(&z, 0, sizeof(z));
  z.zalloc = zalloc;
  z.zfree = zfree;
  if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
    return "inflateInit2";
  z.next_out = out;
  z.avail_out = size;
  int r = Z_OK;
  while (r == Z_OK && done < csize) {
    uint32_t n = csize - done < CHUNK ? csize - done : CHUNK;
    if (archive_read_at(archive, chunk, n, s->start + done) != (int)n)
      break;
    done += n;
    z.next_in = chunk;
    z.avail_in = n;
    r = inflate(&z, done == csize ? Z_FINISH : Z_NO_FLUSH);
  }
  int ok = r == Z_STREAM_END && z.total_in == csize && z.total_out == size;
  inflateEnd(&z);
  if (!ok)
    return done < csize ? "short read" : "inflate";
  uint8_t d[16];
  if (!crc && archive_read_at(archive, d, sizeof(d), s->start + csize) == sizeof(d)) {
    const uint8_t *p = rd32(d) == 0x08074b50 ? d + 4 : d; /* optional signature */
    if (rd32(p + 4) == csize && rd32(p + 8) == size)
      crc = rd32(p);
  }
  return crc && crc32(0, out, size) != crc ? "crc" : NULL;
}

/* Decompresses the stream's entry into the cache. Returns 0 if it's there. */
static int load(const zip_stream *s) {
  uint32_t csize = rd32(s->header + 18), size = entry_size(s);
  if (!size || size > ENTRY_MAX)
    return -1;
  uint32_t t = perf_now();
  sceKernelLockLwMutex(&lock, 1, NULL);
  if (!space || find(s)) {
    int r = space ? 0 : -1;
    sceKernelUnlockLwMutex(&lock, 1);
    return r;
  }
  while (cached_bytes + size > BUDGET && evict_one())
    ;
  entry *e = region_alloc(sizeof(*e));
  uint8_t *out = e ? region_alloc(size) : NULL;
  const char *why = out ? "no archive has it" : "no room";
  for (int archive = -1; out && why && (archive = archive_find_entry(s->header, s->start, archive + 1)) >= 0;)
    why = inflate_entry(s, archive, out, size);
  if (why) {
    static int failures;
    if (failures++ < 8)
      LOG("[zipcache] entry at %u (%u -> %u bytes) not cached: %s", s->start, csize, size, why);
    if (out)
      sceClibMspaceFree(space, out);
    if (e)
      sceClibMspaceFree(space, e);
    sceKernelUnlockLwMutex(&lock, 1);
    return -1;
  }
  *e = (entry){ NULL, lru, s->start, rd32(s->header + 14), csize, size, out };
  if (lru)
    lru->prev = e;
  lru = e;
  cached_bytes += size;
  sceKernelUnlockLwMutex(&lock, 1);
  perf_add(PERF_ZIP_LOAD, t, size);
  return 0;
}

/* Copies from the cached entry at the stream's position; -1 if not cached. */
static int serve(zip_stream *s, void *dst, int n) {
  int got = -1;
  sceKernelLockLwMutex(&lock, 1, NULL);
  entry *e = find(s);
  if (e) {
    uint32_t pos = s->z.total_out;
    got = pos < e->size ? (int)(e->size - pos) : 0;
    if (got > n)
      got = n;
    memcpy(dst, e->data + pos, got > 0 ? got : 0);
    s->z.total_out = pos + got;
    s->z.reserved = SERVED;
  }
  sceKernelUnlockLwMutex(&lock, 1);
  if (got > 0)
    perf_add(PERF_ZIP_HIT, perf_now(), got);
  return got;
}

static int is_cached(const zip_stream *s) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  int hit = find(s) != NULL;
  sceKernelUnlockLwMutex(&lock, 1);
  return hit;
}

/* A stream starting on an entry another stream already started on */
static int seen_before(const zip_stream *s) {
  uint32_t key = s->start ? s->start : 1, i = (key * 2654435761u) % SEEN_SLOTS;
  for (int probe = 0; probe < 16; probe++, i = (i + 1) % SEEN_SLOTS) {
    uint32_t v = __atomic_load_n(&seen[i], __ATOMIC_RELAXED);
    if (v == key)
      return 1;
    if (!v && __atomic_compare_exchange_n(&seen[i], &v, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      return 0;
  }
  return 0;
}

/* Hands a stream the cache gave up on back to Unity: an out-of-range
 * total_out makes Unity's Seek restart the inflate and skip to the position. */
static void give_back(zip_stream *s) {
  uint32_t pos = s->z.total_out;
  s->z.reserved = 0;
  s->z.total_out = 0x7FFFFFFF;
  unity_seek(s, pos, SEEK_SET);
}

static int fresh(const zip_stream *s) {
  return s->z.total_out == 0 && s->pos == 0;
}

static int zs_read(zip_stream *s, void *dst, int n) {
  if (!deflated(s))
    return unity_read(s, dst, n);
  int served = s->z.reserved == SERVED;
  if (served || is_cached(s) || (fresh(s) && seen_before(s) && load(s) == 0)) {
    int got = serve(s, dst, n);
    if (got < 0 && load(s) == 0)
      got = serve(s, dst, n);
    if (got >= 0)
      return got;
    if (served)
      give_back(s);
  }
  return unity_read(s, dst, n);
}

static int zs_seek(zip_stream *s, int off, int whence) {
  if (!deflated(s))
    return unity_seek(s, off, whence);
  /* as Unity computes it, SEEK_END included */
  int target = whence == SEEK_CUR ? (int)s->z.total_out + off : whence == SEEK_END ? (int)entry_size(s) - off : off;
  int served = s->z.reserved == SERVED;
  if (target >= 0 && (served || target < (int)s->z.total_out || is_cached(s) || (fresh(s) && seen_before(s)))) {
    if (is_cached(s) || load(s) == 0) {
      s->z.total_out = target;
      s->z.reserved = SERVED;
      return 0;
    }
  }
  if (served)
    give_back(s);
  return unity_seek(s, off, whence);
}

/* alloc.c's reclaim hook: the whole block, for good */
static size_t release(size_t want) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  size_t freed = 0;
  if (space) {
    sceClibMspaceDestroy(space);
    sceKernelFreeMemBlock(block);
    space = NULL;
    lru = NULL;
    cached_bytes = 0;
    freed = REGION;
  }
  sceKernelUnlockLwMutex(&lock, 1);
  if (freed)
    LOG("[zipcache] off: its %u KB block given back for a %u KB allocation", (unsigned)(freed / 1024),
        (unsigned)(want / 1024));
  return freed;
}

void zipcache_patch(void) {
  uintptr_t base = gamebin_unity->base;
  void **read_slot = (void **)(base + GB_UNITY_ZIPSTREAM_READ_SLOT);
  void **seek_slot = (void **)(base + GB_UNITY_ZIPSTREAM_SEEK_SLOT);
  if ((uintptr_t)*read_slot != base + GB_UNITY_ZIPSTREAM_READ || (uintptr_t)*seek_slot != base + GB_UNITY_ZIPSTREAM_SEEK) {
    LOG("[zipcache] unexpected zip stream vtable, not caching");
    return;
  }
  void *base_addr;
  block = sceKernelAllocMemBlock("pg3d_zipcache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, REGION, NULL);
  if (block < 0 || sceKernelGetMemBlockBase(block, &base_addr) < 0 || !(space = sceClibMspaceCreate(base_addr, REGION)) ||
      !(chunk = sceClibMspaceMalloc(space, CHUNK))) {
    LOG("[zipcache] no memory block (0x%08X), not caching", block);
    return;
  }
  sceKernelCreateLwMutex(&lock, "zipcache", 0, 0, NULL);
  unity_read = (read_fn)*read_slot;
  unity_seek = (seek_fn)*seek_slot;
  void *ours[] = { (void *)zs_read, (void *)zs_seek };
  so_write((uintptr_t)read_slot, &ours[0], sizeof(void *));
  so_write((uintptr_t)seek_slot, &ours[1], sizeof(void *));
  extern void alloc_set_reclaim(size_t (*fn)(size_t));
  alloc_set_reclaim(release);
  LOG("[patch] zip entry streams -> decompressed entry cache (%u KB block)", REGION / 1024);
}
