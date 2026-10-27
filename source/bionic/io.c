/*
 * File IO bridge. fopen/stat/dirent are backed by newlib, with Android path
 * translation and Bionic<->newlib struct conversion. The three std streams
 * arrive as &__sF[0..2]; the stdio wrappers recognise those sentinels and
 * route them to the log.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/rng.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "bionic/archive.h"
#include "bionic/bionic.h"
#include "bionic/net.h"
#include "util/log.h"
#include "util/perf.h"

/* Bionic stdio sentinels. Real newlib FILE* are heap pointers far from this
 * array, so identity comparison is unambiguous. */
unsigned char __sF[3][152];
#define IS_STD(f) ((void *)(f) >= (void *)__sF && (void *)(f) < (void *)(__sF + 3))
#define STD_IDX(f) ((int)(((uintptr_t)(f) - (uintptr_t)__sF) / sizeof(__sF[0])))

/* fds opened on /dev/random or /dev/urandom */
static uint8_t rng_fd[256];

static void mappath(const char *p, char *buf, size_t n) {
  bionic_map_path(p, buf, n);
}

FILE *b_fopen(const char *path, const char *mode) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  uint32_t t = perf_now();
  int reading = strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0;
  FILE *f = reading ? archive_fopen(vp) : NULL;
  if (!f)
    f = fopen(vp, mode);
  if (!f) {
    bionic_errno_sync();
    perf_add(PERF_FOPEN_FAIL, t, 0);
    DLOG("[io] fopen(%s, %s) failed", vp, mode);
    return NULL;
  }
  perf_add(PERF_FOPEN, t, 0);
  perf_files(1);
  return f;
}

FILE *b_fdopen(int fd, const char *mode) {
  return fdopen(fd, mode);
}

FILE *b_freopen(const char *path, const char *mode, FILE *f) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  return freopen(vp, mode, IS_STD(f) ? NULL : f);
}

int b_fclose(FILE *f) {
  if (IS_STD(f))
    return 0;
  uint32_t t = perf_now();
  int r = fclose(f);
  perf_add(PERF_FCLOSE, t, 0);
  perf_files(-1);
  return r;
}

size_t b_fread(void *p, size_t s, size_t n, FILE *f) {
  if (IS_STD(f))
    return 0;
  uint32_t t = perf_now();
  size_t r = fread(p, s, n, f);
  perf_add(PERF_FREAD, t, r * s);
  return r;
}

size_t b_fwrite(const void *p, size_t s, size_t n, FILE *f) {
  if (IS_STD(f)) {
    log_printf("%.*s", (int)(s * n), (const char *)p);
    return n;
  }
  return fwrite(p, s, n, f);
}

int b_fseek(FILE *f, long off, int whence) {
  if (IS_STD(f))
    return 0;
  uint32_t t = perf_now();
  int r = fseek(f, off, whence);
  perf_add(PERF_FSEEK, t, 0);
  return r;
}

long b_ftell(FILE *f) {
  if (IS_STD(f))
    return 0;
  return ftell(f);
}

int b_fflush(FILE *f) {
  if (!f || IS_STD(f)) {
    log_flush();
    return 0;
  }
  return fflush(f);
}

int b_ferror(FILE *f) {
  if (IS_STD(f))
    return 0;
  return ferror(f);
}

int b_feof(FILE *f) {
  if (IS_STD(f))
    return 0;
  return feof(f);
}

char *b_fgets(char *s, int n, FILE *f) {
  if (IS_STD(f))
    return NULL;
  return fgets(s, n, f);
}

int b_fputs(const char *s, FILE *f) {
  if (IS_STD(f)) {
    log_printf("%s", s);
    return 0;
  }
  return fputs(s, f);
}

int b_fputc(int c, FILE *f) {
  if (IS_STD(f)) {
    char ch = c;
    log_printf("%c", ch);
    return c;
  }
  return fputc(c, f);
}

int b_putc(int c, FILE *f) {
  return b_fputc(c, f);
}

int b_getc(FILE *f) {
  if (IS_STD(f))
    return -1;
  return getc(f);
}

int b_ungetc(int c, FILE *f) {
  if (IS_STD(f))
    return c;
  return ungetc(c, f);
}

int b_setvbuf(FILE *f, char *buf, int mode, size_t size) {
  if (IS_STD(f))
    return 0;
  return setvbuf(f, buf, mode, size);
}

int b_fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (IS_STD(f)) {
    log_vprintf(fmt, ap);
    va_end(ap);
    return 0;
  }
  int r = vfprintf(f, fmt, ap);
  va_end(ap);
  return r;
}

int b_vfprintf(FILE *f, const char *fmt, va_list ap) {
  if (IS_STD(f)) {
    log_vprintf(fmt, ap);
    return 0;
  }
  return vfprintf(f, fmt, ap);
}

int b_fscanf(FILE *f, const char *fmt, ...) {
  if (IS_STD(f))
    return -1;
  va_list ap;
  va_start(ap, fmt);
  int r = vfscanf(f, fmt, ap);
  va_end(ap);
  return r;
}

/* ---------- low-level fd IO ---------- */

int b_open(const char *path, int flags, ...) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  va_list ap;
  va_start(ap, flags);
  int mode = va_arg(ap, int);
  va_end(ap);
  /* bionic O_* numeric values differ only for O_DIRECTORY/O_CLOEXEC/O_LARGEFILE;
   * the low bits (RDONLY/WRONLY/RDWR/CREAT/TRUNC/APPEND/EXCL/NONBLOCK) match. */
  int nf = flags & 0x30603; /* keep standard low flags */
  if (flags & 0x4000) nf |= O_NONBLOCK;
  int fd = open(vp, nf, mode);
  if (fd < 0)
    bionic_errno_sync();
  else if (fd < 256)
    rng_fd[fd] = strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0;
  DLOG("[io] open(%s -> %s, 0x%X) = %d", path, vp, flags, fd);
  return fd;
}


static int is_rng(int fd) {
  return fd >= 0 && fd < 256 && rng_fd[fd];
}

int b_close(int fd) {
  if (net_is_fd(fd))
    return net_close(fd);
  if (fd >= 0 && fd < 256)
    rng_fd[fd] = 0;
  return close(fd);
}

long b_read(int fd, void *b, size_t n) {
  if (net_is_fd(fd))
    return net_read(fd, b, n);
  if (is_rng(fd)) {
    for (size_t done = 0; done < n; done += 64)
      sceKernelGetRandomNumber((uint8_t *)b + done, n - done < 64 ? n - done : 64);
    return n;
  }
  return read(fd, b, n);
}
long b_write(int fd, const void *b, size_t n) {
  if (net_is_fd(fd))
    return net_write(fd, b, n);
  return write(fd, b, n);
}
long b_lseek(int fd, long off, int w) { return lseek(fd, off, w); }

int b_access(const char *path, int mode) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  int r = access(vp, mode);
  if (r < 0)
    bionic_errno_sync();
  return r;
}

int b_unlink(const char *path) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  return unlink(vp);
}

int b_rename(const char *a, const char *b) {
  char va[512], vb[512];
  mappath(a, va, sizeof(va));
  mappath(b, vb, sizeof(vb));
  return rename(va, vb);
}

int b_mkdir(const char *path, int mode) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  int r = mkdir(vp, mode);
  if (r < 0 && errno == EEXIST)
    return 0;
  return r;
}

int b_rmdir(const char *path) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  return rmdir(vp);
}

int b_chdir(const char *path) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  return chdir(vp);
}

char *b_getcwd(char *buf, size_t n) {
  if (buf && n) {
    snprintf(buf, n, "/data/data/com.pixel.gun3d/files");
    return buf;
  }
  return NULL;
}

/* ---------- stat ---------- */

struct bionic_stat {
  uint64_t st_dev;
  uint32_t __pad0;
  uint32_t __st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint32_t __pad3;
  int64_t st_size;
  uint32_t st_blksize;
  uint64_t st_blocks;
  uint32_t st_atime_;
  uint32_t st_atime_nsec;
  uint32_t st_mtime_;
  uint32_t st_mtime_nsec;
  uint32_t st_ctime_;
  uint32_t st_ctime_nsec;
  uint64_t st_ino;
};

static void conv_stat(const struct stat *s, struct bionic_stat *b) {
  memset(b, 0, sizeof(*b));
  b->st_dev = s->st_dev;
  b->st_ino = b->__st_ino = s->st_ino;
  b->st_mode = s->st_mode;
  b->st_nlink = s->st_nlink;
  b->st_uid = s->st_uid;
  b->st_gid = s->st_gid;
  b->st_rdev = s->st_rdev;
  b->st_size = s->st_size;
  b->st_blksize = s->st_blksize ? s->st_blksize : 4096;
  b->st_blocks = (s->st_size + 511) / 512;
  b->st_atime_ = s->st_atime;
  b->st_mtime_ = s->st_mtime;
  b->st_ctime_ = s->st_ctime;
}

int b_stat(const char *path, struct bionic_stat *b) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  struct stat s;
  int r = stat(vp, &s);
  if (r < 0) {
    bionic_errno_sync();
    return r;
  }
  conv_stat(&s, b);
  return 0;
}

int b_lstat(const char *path, struct bionic_stat *b) {
  return b_stat(path, b);
}

int b_fstat(int fd, struct bionic_stat *b) {
  if (net_is_fd(fd)) {
    memset(b, 0, sizeof(*b));
    b->st_mode = 0140666; /* S_IFSOCK */
    return 0;
  }
  struct stat s;
  int r = fstat(fd, &s);
  if (r < 0) {
    bionic_errno_sync();
    return r;
  }
  conv_stat(&s, b);
  return 0;
}

/* ---------- dirent ---------- */

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

typedef struct {
  DIR *dir;
  struct bionic_dirent ent;
} bionic_DIR;

void *b_opendir(const char *path) {
  char vp[512];
  mappath(path, vp, sizeof(vp));
  DIR *d = opendir(vp);
  if (!d) {
    bionic_errno_sync();
    return NULL;
  }
  bionic_DIR *bd = calloc(1, sizeof(*bd));
  bd->dir = d;
  return bd;
}

void *b_readdir(void *p) {
  bionic_DIR *bd = p;
  struct dirent *e = readdir(bd->dir);
  if (!e)
    return NULL;
  memset(&bd->ent, 0, sizeof(bd->ent));
  strncpy(bd->ent.d_name, e->d_name, sizeof(bd->ent.d_name) - 1);
  bd->ent.d_reclen = sizeof(bd->ent);
#ifdef DT_DIR
  bd->ent.d_type = e->d_type;
#endif
  return &bd->ent;
}

int b_closedir(void *p) {
  bionic_DIR *bd = p;
  int r = closedir(bd->dir);
  free(bd);
  return r;
}

/* ---------- wide stdio (the game only moves ASCII through these) ---------- */

unsigned b_getwc(FILE *f) {
  int c = b_getc(f);
  return c < 0 ? 0xffffffffu : (unsigned)c;
}

unsigned b_putwc(unsigned wc, FILE *f) {
  return b_fputc(wc < 0x80 ? (int)wc : '?', f) < 0 ? 0xffffffffu : wc;
}

unsigned b_ungetwc(unsigned wc, FILE *f) {
  return b_ungetc((int)wc, f) < 0 ? 0xffffffffu : wc;
}

/* ---------- counted paths (util/perf.h) ---------- */

/* Unity's zip streams (APK and OBB entries) */
int b_inflate(z_streamp strm, int flush) {
  uint32_t t = perf_now(), room = strm->avail_out;
  int r = inflate(strm, flush);
  perf_add(PERF_INFLATE, t, room - strm->avail_out);
  return r;
}

/* newlib's own syscalls, under every stream (linked with --wrap) */
struct _reent;
int __real__open_r(struct _reent *r, const char *path, int flags, int mode);
int __real__read_r(struct _reent *r, int fd, void *buf, size_t n);
long __real__lseek_r(struct _reent *r, int fd, long off, int whence);
int __real__fstat_r(struct _reent *r, int fd, struct stat *st);
int __real__close_r(struct _reent *r, int fd);

int __wrap__open_r(struct _reent *r, const char *path, int flags, int mode) {
  uint32_t t = perf_now();
  int fd = __real__open_r(r, path, flags, mode);
  perf_add(PERF_SYS_OPEN, t, 0);
  return fd;
}

int __wrap__read_r(struct _reent *r, int fd, void *buf, size_t n) {
  uint32_t t = perf_now();
  int got = __real__read_r(r, fd, buf, n);
  perf_add(PERF_SYS_READ, t, got > 0 ? got : 0);
  return got;
}

long __wrap__lseek_r(struct _reent *r, int fd, long off, int whence) {
  uint32_t t = perf_now();
  long pos = __real__lseek_r(r, fd, off, whence);
  perf_add(PERF_SYS_LSEEK, t, 0);
  return pos;
}

int __wrap__fstat_r(struct _reent *r, int fd, struct stat *st) {
  uint32_t t = perf_now();
  int ret = __real__fstat_r(r, fd, st);
  perf_add(PERF_SYS_FSTAT, t, 0);
  return ret;
}

int __wrap__close_r(struct _reent *r, int fd) {
  uint32_t t = perf_now();
  int ret = __real__close_r(r, fd);
  perf_add(PERF_SYS_CLOSE, t, 0);
  return ret;
}
