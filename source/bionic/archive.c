/*
 * Read-only streams over the game's archives (base.apk and the OBB).
 *
 * Unity opens an archive for every zip entry it reads and reads it as fseek +
 * fread. On newlib each open is an sceIoOpen, each read goes through a 1 KB
 * buffer (one sceIoRead per KB) and each seek refills that buffer. Here an
 * archive is opened once and every stream reads it with sceIoPread at its
 * own position, through a read-ahead that grows while reads are sequential.
 *
 * At boot Unity also reads the 30-byte local header of every entry: 12k
 * scattered reads across the OBB, about 10 s. Small reads that had to go to
 * the card are remembered and saved as <data root>/<archive>.reads, and later
 * boots answer them from memory. The file is tied to the archive's size and
 * modification time.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/archive.h"
#include "bionic/bionic.h"
#include "util/log.h"
#include "util/perf.h"

#define MAX_ARCHIVES 4
#define AHEAD_MIN 1024
#define AHEAD_MAX (64 * 1024) /* reads this big skip the read-ahead */
#define SMALL 32              /* reads up to this size are remembered */
#define EMPTY 0xFFFFFFFFu
#define READS_MAGIC 0x31524750 /* "PGR1" */

typedef struct {
  uint32_t off;
  uint8_t len, data[SMALL];
} small_read;

typedef struct {
  char path[256];
  SceUID fd;
  uint32_t size;
  SceDateTime mtime;
  SceKernelLwMutexWork lock; /* guards the small reads */
  small_read *reads;
  uint32_t mask, count;
  int dirty;
} archive;

#define POOL 8 /* read-ahead buffers kept for the streams, so opens don't churn the heap */

static archive archives[MAX_ARCHIVES];
static uint8_t pool[POOL][AHEAD_MAX] __attribute__((aligned(64)));
static uint32_t pool_used;
static int narchives;
static SceKernelLwMutexWork registry_lock;

typedef struct {
  archive *a;
  uint32_t pos, last_end, ahead;
  uint32_t buf_off, buf_len;
  uint8_t *buf;
} stream;

void archive_init(void) {
  sceKernelCreateLwMutex(&registry_lock, "archives", 0, 0, NULL);
}

static int is_archive(const char *path) {
  size_t n = strlen(path);
  return (n > 4 && strcmp(path + n - 4, ".obb") == 0) || (n > 9 && strcmp(path + n - 9, "/base.apk") == 0);
}

/* ---------- small reads ---------- */

static small_read *slot(archive *a, uint32_t off) {
  uint32_t i = (off * 2654435761u) & a->mask;
  while (a->reads[i].off != EMPTY && a->reads[i].off != off)
    i = (i + 1) & a->mask;
  return &a->reads[i];
}

static int reads_resize(archive *a, uint32_t cap) {
  small_read *old = a->reads;
  uint32_t old_cap = old ? a->mask + 1 : 0;
  small_read *r = malloc(cap * sizeof(*r));
  if (!r)
    return -1;
  for (uint32_t i = 0; i < cap; i++)
    r[i].off = EMPTY;
  a->reads = r;
  a->mask = cap - 1;
  for (uint32_t i = 0; i < old_cap; i++)
    if (old[i].off != EMPTY)
      *slot(a, old[i].off) = old[i];
  free(old);
  return 0;
}

static int reads_get(archive *a, uint32_t off, void *dst, uint32_t n) {
  int hit = 0;
  sceKernelLockLwMutex(&a->lock, 1, NULL);
  if (a->reads) {
    small_read *s = slot(a, off);
    if (s->off == off && s->len >= n) {
      memcpy(dst, s->data, n);
      hit = 1;
    }
  }
  sceKernelUnlockLwMutex(&a->lock, 1);
  return hit;
}

static void reads_put(archive *a, uint32_t off, const void *src, uint32_t n) {
  sceKernelLockLwMutex(&a->lock, 1, NULL);
  if (!a->reads || (a->count + 1) * 2 > a->mask + 1)
    reads_resize(a, a->reads ? (a->mask + 1) * 2 : 1024);
  if ((a->count + 1) * 2 <= a->mask + 1) {
    small_read *s = slot(a, off);
    if (s->off == EMPTY) {
      s->off = off;
      s->len = 0;
      a->count++;
    }
    if (n > s->len) {
      memcpy(s->data, src, n);
      s->len = n;
      a->dirty = 1;
    }
  }
  sceKernelUnlockLwMutex(&a->lock, 1);
}

typedef struct {
  uint32_t magic, size;
  SceDateTime mtime;
  uint32_t count;
} reads_file;

static void reads_path(const archive *a, char *out, size_t n) {
  const char *base = strrchr(a->path, '/');
  snprintf(out, n, "%s/%s.reads", bionic_data_root(), base ? base + 1 : a->path);
}

static void reads_load(archive *a) {
  char path[300];
  reads_path(a, path, sizeof(path));
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return;
  reads_file h;
  uint8_t *body = NULL;
  int size = sceIoLseek32(fd, 0, SCE_SEEK_END);
  sceIoLseek32(fd, 0, SCE_SEEK_SET);
  if (size > (int)sizeof(h) && sceIoRead(fd, &h, sizeof(h)) == sizeof(h) && h.magic == READS_MAGIC &&
      h.size == a->size && memcmp(&h.mtime, &a->mtime, sizeof(h.mtime)) == 0 && (body = malloc(size - sizeof(h))) &&
      sceIoRead(fd, body, size - sizeof(h)) == size - (int)sizeof(h)) {
    uint32_t cap = 1024;
    while (cap < h.count * 2 + 2)
      cap *= 2;
    if (reads_resize(a, cap) == 0) {
      uint8_t *p = body, *end = body + size - sizeof(h);
      for (uint32_t i = 0; i < h.count && p + 5 <= end; i++) {
        uint32_t off;
        memcpy(&off, p, 4);
        uint8_t len = p[4];
        if (len > SMALL || p + 5 + len > end)
          break;
        small_read *s = slot(a, off);
        if (s->off == EMPTY)
          a->count++;
        s->off = off;
        s->len = len;
        memcpy(s->data, p + 5, len);
        p += 5 + len;
      }
    }
  }
  free(body);
  sceIoClose(fd);
  LOG("[archive] %s: %u remembered small reads", a->path, a->count);
}

static void reads_save(archive *a) {
  sceKernelLockLwMutex(&a->lock, 1, NULL);
  uint32_t cap = a->mask + 1, n = 0;
  uint8_t *out = a->dirty ? malloc(sizeof(reads_file) + a->count * (5 + SMALL)) : NULL;
  if (out) {
    uint8_t *p = out + sizeof(reads_file);
    for (uint32_t i = 0; i < cap; i++) {
      small_read *s = &a->reads[i];
      if (s->off == EMPTY)
        continue;
      memcpy(p, &s->off, 4);
      p[4] = s->len;
      memcpy(p + 5, s->data, s->len);
      p += 5 + s->len;
      n++;
    }
    reads_file h = { READS_MAGIC, a->size, a->mtime, n };
    memcpy(out, &h, sizeof(h));
    a->dirty = 0;
    sceKernelUnlockLwMutex(&a->lock, 1);
    char path[300];
    reads_path(a, path, sizeof(path));
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
      sceIoWrite(fd, out, p - out);
      sceIoClose(fd);
    }
    free(out);
    LOG("[archive] saved %u small reads of %s", n, a->path);
    return;
  }
  sceKernelUnlockLwMutex(&a->lock, 1);
}

void archive_after_load(void) {
  for (int i = 0; i < narchives; i++)
    if (archives[i].dirty)
      reads_save(&archives[i]);
}

/* ---------- archives ---------- */

static archive *archive_get(const char *path) {
  archive *a = NULL;
  sceKernelLockLwMutex(&registry_lock, 1, NULL);
  for (int i = 0; i < narchives && !a; i++)
    if (strcmp(archives[i].path, path) == 0)
      a = &archives[i];
  if (!a && narchives < MAX_ARCHIVES) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceIoStat st;
    if (fd >= 0 && sceIoGetstatByFd(fd, &st) >= 0) {
      a = &archives[narchives];
      snprintf(a->path, sizeof(a->path), "%s", path);
      a->fd = fd;
      a->size = (uint32_t)st.st_size;
      a->mtime = st.st_mtime;
      sceKernelCreateLwMutex(&a->lock, "archive", 0, 0, NULL);
      reads_load(a);
      narchives++;
    } else if (fd >= 0) {
      sceIoClose(fd);
    }
  }
  sceKernelUnlockLwMutex(&registry_lock, 1);
  return a;
}

static int pread_full(archive *a, void *dst, uint32_t n, uint32_t off) {
  uint32_t done = 0;
  while (done < n) {
    uint32_t t = perf_now();
    int r = sceIoPread(a->fd, (uint8_t *)dst + done, n - done, off + done);
    perf_add(PERF_SYS_READ, t, r > 0 ? r : 0);
    if (r < 0)
      return done ? (int)done : -1;
    if (r == 0)
      break;
    done += r;
  }
  return done;
}

int archive_read_at(int i, void *dst, uint32_t size, uint32_t offset) {
  if (i < 0 || i >= narchives)
    return -1;
  return pread_full(&archives[i], dst, size, offset);
}

int archive_find_entry(const uint8_t *header, uint32_t data_start, int from) {
  uint16_t name, extra;
  memcpy(&name, header + 26, 2);
  memcpy(&extra, header + 28, 2);
  uint32_t at = data_start - 30 - name - extra;
  for (int i = from < 0 ? 0 : from; i < narchives; i++) {
    uint8_t h[30];
    if (at >= archives[i].size || pread_full(&archives[i], h, sizeof(h), at) != sizeof(h))
      continue;
    /* signature, method, name and extra lengths; entries written with a data
     * descriptor (flag bit 3) leave CRC and sizes zero here */
    if (memcmp(h, "PK\3\4", 4) != 0 || memcmp(h + 8, header + 8, 2) != 0 || memcmp(h + 26, header + 26, 4) != 0)
      continue;
    if (!(h[6] & 8) && memcmp(h + 14, header + 14, 12) != 0)
      continue;
    return i;
  }
  return -1;
}

/* ---------- streams ---------- */

static uint8_t *buffer_get(void) {
  for (int i = 0; i < POOL; i++) {
    uint32_t bit = 1u << i;
    if (!(__atomic_fetch_or(&pool_used, bit, __ATOMIC_ACQUIRE) & bit))
      return pool[i];
  }
  return malloc(AHEAD_MAX);
}

static void buffer_put(uint8_t *b) {
  if (b >= pool[0] && b < pool[POOL])
    __atomic_fetch_and(&pool_used, ~(1u << ((b - pool[0]) / AHEAD_MAX)), __ATOMIC_RELEASE);
  else
    free(b);
}

static int stream_read(void *cookie, char *dst, int n) {
  stream *s = cookie;
  archive *a = s->a;
  if (n <= 0 || s->pos >= a->size)
    return 0;
  if ((uint32_t)n > a->size - s->pos)
    n = a->size - s->pos;
  int sequential = s->pos == s->last_end;
  uint32_t done = 0;
  if (s->buf_len && s->pos >= s->buf_off && s->pos < s->buf_off + s->buf_len) {
    uint32_t k = s->buf_off + s->buf_len - s->pos;
    if (k > (uint32_t)n)
      k = n;
    memcpy(dst, s->buf + (s->pos - s->buf_off), k);
    s->pos += k;
    done = k;
  }
  uint32_t rest = n - done;
  if (rest && !done && !sequential && rest <= SMALL && reads_get(a, s->pos, dst, rest)) {
    s->pos += rest;
    done = rest;
    rest = 0;
  }
  if (rest >= AHEAD_MAX) {
    int r = pread_full(a, dst + done, rest, s->pos);
    if (r < 0)
      return done ? (int)done : -1;
    s->pos += r;
    done += r;
  } else if (rest) {
    if (!s->buf && !(s->buf = buffer_get()))
      return done ? (int)done : -1;
    uint32_t want = sequential ? s->ahead * 2 : AHEAD_MIN;
    if (want < rest)
      want = rest;
    if (want > AHEAD_MAX)
      want = AHEAD_MAX;
    if (want > a->size - s->pos)
      want = a->size - s->pos;
    s->ahead = want;
    int r = pread_full(a, s->buf, want, s->pos);
    if (r < 0)
      return done ? (int)done : -1;
    s->buf_off = s->pos;
    s->buf_len = r;
    uint32_t k = (uint32_t)r < rest ? (uint32_t)r : rest;
    memcpy(dst + done, s->buf, k);
    if (!done && !sequential && rest <= SMALL && k == rest)
      reads_put(a, s->pos, s->buf, k);
    s->pos += k;
    done += k;
  }
  s->last_end = s->pos;
  return done;
}

static fpos_t stream_seek(void *cookie, fpos_t off, int whence) {
  stream *s = cookie;
  long pos = whence == SEEK_SET ? off : whence == SEEK_CUR ? (long)s->pos + off : (long)s->a->size + off;
  if (pos < 0) {
    errno = EINVAL;
    return -1;
  }
  s->pos = pos;
  return pos;
}

static int stream_close(void *cookie) {
  stream *s = cookie;
  if (s->buf)
    buffer_put(s->buf);
  free(s);
  return 0;
}

FILE *archive_fopen(const char *path) {
  if (!is_archive(path))
    return NULL;
  archive *a = archive_get(path);
  stream *s = a ? calloc(1, sizeof(*s)) : NULL;
  if (!s)
    return NULL;
  s->a = a;
  s->last_end = EMPTY;
  FILE *f = funopen(s, stream_read, NULL, stream_seek, stream_close);
  if (!f) {
    free(s);
    return NULL;
  }
  /* reads go straight to stream_read with the caller's size */
  setvbuf(f, NULL, _IONBF, 0);
  return f;
}
