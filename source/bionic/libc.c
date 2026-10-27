/*
 * Remaining libc surface: struct tm (bionic adds tm_gmtoff/tm_zone), wide
 * chars with bionic's own wctype ids, environment, strerror over Linux errno,
 * stdout helpers, process control (refused) and fd odds and ends.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <psp2/io/devctl.h>
#include <psp2/io/stat.h>

#include "bionic/bionic.h"
#include "bionic/net.h"
#include "util/log.h"

/* ---------- struct tm ---------- */

struct bionic_tm {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  long tm_gmtoff;
  const char *tm_zone;
};

static void tm_out(const struct tm *s, struct bionic_tm *d) {
  memcpy(d, s, 9 * sizeof(int));
  d->tm_gmtoff = 0;
  d->tm_zone = "UTC";
}

static void tm_in(const struct bionic_tm *s, struct tm *d) {
  memset(d, 0, sizeof(*d));
  memcpy(d, s, 9 * sizeof(int));
}

static struct bionic_tm static_tm;

struct bionic_tm *b_gmtime(const int32_t *t) {
  time_t tt = *t;
  struct tm r;
  gmtime_r(&tt, &r);
  tm_out(&r, &static_tm);
  return &static_tm;
}

/* The Vita clock is kept in UTC by bionic_realtime_us, and the game only uses
 * local time for display, so local time is UTC. */
struct bionic_tm *b_localtime(const int32_t *t) {
  return b_gmtime(t);
}

int32_t b_mktime(struct bionic_tm *bt) {
  struct tm t;
  tm_in(bt, &t);
  time_t r = mktime(&t);
  tm_out(&t, bt);
  return (int32_t)r;
}

double b_difftime(int32_t a, int32_t b) {
  return (double)a - (double)b;
}

size_t b_strftime(char *s, size_t max, const char *fmt, const struct bionic_tm *bt) {
  struct tm t;
  tm_in(bt, &t);
  return strftime(s, max, fmt, &t);
}

size_t b_wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct bionic_tm *bt) {
  struct tm t;
  tm_in(bt, &t);
  return wcsftime(s, max, fmt, &t);
}

/* ---------- wide characters ---------- */

/* bionic's mbstate_t is 4 bytes, newlib's is 8: never let newlib touch the
 * caller's state. UTF-8 is decoded statelessly; truncated input reports -2. */
size_t b_mbrtowc(wchar_t *pwc, const char *s, size_t n, void *ps) {
  if (!s)
    return 0;
  if (n == 0)
    return (size_t)-2;
  const unsigned char *u = (const unsigned char *)s;
  unsigned c = u[0];
  size_t len;
  unsigned wc;
  if (c < 0x80) {
    len = 1;
    wc = c;
  } else if ((c & 0xe0) == 0xc0) {
    len = 2;
    wc = c & 0x1f;
  } else if ((c & 0xf0) == 0xe0) {
    len = 3;
    wc = c & 0x0f;
  } else if ((c & 0xf8) == 0xf0) {
    len = 4;
    wc = c & 0x07;
  } else {
    bionic_set_errno(84); /* EILSEQ */
    return (size_t)-1;
  }
  if (n < len)
    return (size_t)-2;
  for (size_t i = 1; i < len; i++) {
    if ((u[i] & 0xc0) != 0x80) {
      bionic_set_errno(84);
      return (size_t)-1;
    }
    wc = (wc << 6) | (u[i] & 0x3f);
  }
  if (pwc)
    *pwc = wc;
  return wc ? len : 0;
}

size_t b_wcrtomb(char *s, wchar_t wc, void *ps) {
  char tmp[4];
  if (!s) {
    s = tmp;
    wc = 0;
  }
  unsigned c = wc;
  if (c < 0x80) {
    s[0] = c;
    return 1;
  }
  if (c < 0x800) {
    s[0] = 0xc0 | (c >> 6);
    s[1] = 0x80 | (c & 0x3f);
    return 2;
  }
  if (c < 0x10000) {
    s[0] = 0xe0 | (c >> 12);
    s[1] = 0x80 | ((c >> 6) & 0x3f);
    s[2] = 0x80 | (c & 0x3f);
    return 3;
  }
  if (c < 0x110000) {
    s[0] = 0xf0 | (c >> 18);
    s[1] = 0x80 | ((c >> 12) & 0x3f);
    s[2] = 0x80 | ((c >> 6) & 0x3f);
    s[3] = 0x80 | (c & 0x3f);
    return 4;
  }
  bionic_set_errno(84);
  return (size_t)-1;
}

int b_wctob(unsigned c) {
  return c < 0x80 ? (int)c : -1;
}

unsigned b_btowc(int c) {
  return (c >= 0 && c < 0x80) ? (unsigned)c : 0xffffffffu; /* WEOF */
}

/* wctype ids are opaque to callers: keep our own numbering. */
static const char *const wctype_names[] = { NULL,    "alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                            "lower", "print", "punct", "space", "upper", "xdigit" };

unsigned long b_wctype(const char *name) {
  for (unsigned i = 1; i < sizeof(wctype_names) / sizeof(wctype_names[0]); i++)
    if (strcmp(name, wctype_names[i]) == 0)
      return i;
  return 0;
}

int b_iswctype(unsigned wc, unsigned long t) {
  switch (t) {
  case 1: return iswalnum(wc);
  case 2: return iswalpha(wc);
  case 3: return iswblank(wc);
  case 4: return iswcntrl(wc);
  case 5: return iswdigit(wc);
  case 6: return iswgraph(wc);
  case 7: return iswlower(wc);
  case 8: return iswprint(wc);
  case 9: return iswpunct(wc);
  case 10: return iswspace(wc);
  case 11: return iswupper(wc);
  case 12: return iswxdigit(wc);
  default: return 0;
  }
}

/* ---------- environment ---------- */

#define ENV_MAX 32
static char *env_k[ENV_MAX], *env_v[ENV_MAX];

char *b_getenv(const char *name) {
  for (int i = 0; i < ENV_MAX; i++)
    if (env_k[i] && strcmp(env_k[i], name) == 0)
      return env_v[i];
  /* once per name: the GC asks on every collection */
  static const char *asked[16];
  for (int i = 0; i < 16; i++) {
    if (asked[i] == name)
      break;
    if (!asked[i]) {
      asked[i] = name;
      DLOG("[env] getenv(%s) unset", name);
      break;
    }
  }
  return NULL;
}

int b_setenv(const char *name, const char *value, int overwrite) {
  int free_slot = -1;
  for (int i = 0; i < ENV_MAX; i++) {
    if (env_k[i] && strcmp(env_k[i], name) == 0) {
      if (overwrite) {
        free(env_v[i]);
        env_v[i] = strdup(value);
      }
      return 0;
    }
    if (!env_k[i] && free_slot < 0)
      free_slot = i;
  }
  if (free_slot < 0) {
    bionic_set_errno(12);
    return -1;
  }
  env_k[free_slot] = strdup(name);
  env_v[free_slot] = strdup(value);
  return 0;
}

int b_unsetenv(const char *name) {
  for (int i = 0; i < ENV_MAX; i++) {
    if (env_k[i] && strcmp(env_k[i], name) == 0) {
      free(env_k[i]);
      free(env_v[i]);
      env_k[i] = env_v[i] = NULL;
    }
  }
  return 0;
}

/* ---------- strerror over Linux errno ---------- */

char *b_strerror(int e) {
  static char buf[32];
  switch (e) {
  case 0: return "Success";
  case 1: return "Operation not permitted";
  case 2: return "No such file or directory";
  case 4: return "Interrupted system call";
  case 5: return "I/O error";
  case 9: return "Bad file descriptor";
  case 11: return "Try again";
  case 12: return "Out of memory";
  case 13: return "Permission denied";
  case 16: return "Device or resource busy";
  case 17: return "File exists";
  case 20: return "Not a directory";
  case 21: return "Is a directory";
  case 22: return "Invalid argument";
  case 24: return "Too many open files";
  case 28: return "No space left on device";
  case 32: return "Broken pipe";
  case 38: return "Function not implemented";
  case 101: return "Network is unreachable";
  case 110: return "Connection timed out";
  case 111: return "Connection refused";
  default:
    snprintf(buf, sizeof(buf), "Unknown error %d", e);
    return buf;
  }
}

/* ---------- stdout ---------- */

int b_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_vprintf(fmt, ap);
  va_end(ap);
  return 0;
}

int b_vprintf(const char *fmt, va_list ap) {
  log_vprintf(fmt, ap);
  return 0;
}

int b_puts(const char *s) {
  log_printf("%s", s);
  return 0;
}

int b_putchar(int c) {
  if (c != '\n')
    log_printf("%c", c);
  return c;
}

void b_perror(const char *s) {
  log_printf("%s: %s", s ? s : "", b_strerror(errno));
}

/* ---------- process control: no processes on Vita ---------- */

int b_fork(void) {
  LOG("[proc] fork refused lr=%p", __builtin_return_address(0));
  bionic_set_errno(38);
  return -1;
}

int b_execl(const char *path, const char *arg, ...) {
  LOG("[proc] execl(%s) refused", path);
  bionic_set_errno(38);
  return -1;
}

int b_waitpid(int pid, int *status, int options) {
  bionic_set_errno(10); /* ECHILD */
  return -1;
}

long b_ptrace(int req, int pid, void *addr, void *data) {
  bionic_set_errno(1);
  return -1;
}

/* ---------- fd odds and ends ---------- */

#define B_F_GETFD 1
#define B_F_SETFD 2
#define B_F_GETFL 3
#define B_F_SETFL 4

int b_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  int arg = va_arg(ap, int);
  va_end(ap);
  if (net_is_fd(fd))
    return net_fcntl(fd, cmd, arg);
  switch (cmd) {
  case B_F_GETFD:
  case B_F_SETFD:
    return 0;
  case B_F_GETFL:
    return 2; /* O_RDWR */
  case B_F_SETFL:
    return 0;
  default:
    LOG("[io] fcntl(%d, %d, 0x%X) unsupported", fd, cmd, arg);
    bionic_set_errno(22);
    return -1;
  }
}

int b_ioctl(int fd, int req, ...) {
  if (net_is_fd(fd)) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return net_ioctl(fd, req, arg);
  }
  LOG("[io] ioctl(%d, 0x%X) unsupported lr=%p", fd, req, __builtin_return_address(0));
  bionic_set_errno(25); /* ENOTTY */
  return -1;
}

int b_dup2(int a, int b) {
  LOG("[io] dup2(%d, %d) unsupported", a, b);
  bionic_set_errno(9);
  return -1;
}

int b_flock(int fd, int op) {
  return 0;
}

int b_ftruncate(int fd, long len) {
  int r = ftruncate(fd, len);
  if (r < 0)
    bionic_errno_sync();
  return r;
}

int b_truncate(const char *path, long len) {
  char vp[512];
  bionic_map_path(path, vp, sizeof(vp));
  int r = truncate(vp, len);
  if (r < 0)
    bionic_errno_sync();
  return r;
}

int b_utime(const char *path, const void *times) {
  return 0;
}

struct b_iovec {
  void *base;
  size_t len;
};

long b_writev(int fd, const struct b_iovec *iov, int n) {
  long total = 0;
  for (int i = 0; i < n; i++) {
    long r = net_is_fd(fd) ? net_write(fd, iov[i].base, iov[i].len) : write(fd, iov[i].base, iov[i].len);
    if (r < 0) {
      if (!net_is_fd(fd))
        bionic_errno_sync();
      return total ? total : -1;
    }
    total += r;
  }
  return total;
}

int b_pipe(int fds[2]) {
  int r = pipe(fds);
  if (r < 0)
    bionic_errno_sync();
  return r;
}

/* bionic arm struct statfs (statfs64 body) */
struct bionic_statfs {
  uint32_t f_type;
  uint32_t f_bsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t f_fsid[2];
  uint32_t f_namelen, f_frsize, f_flags;
  uint32_t f_spare[4];
};

int b_statfs(const char *path, struct bionic_statfs *s) {
  memset(s, 0, sizeof(*s));
  SceIoDevInfo info;
  memset(&info, 0, sizeof(info));
  uint64_t free_b = 1024ull * 1024 * 1024, total_b = 4ull * 1024 * 1024 * 1024;
  if (sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info)) >= 0) {
    free_b = info.free_size;
    total_b = info.max_size;
  }
  s->f_type = 0xEF53;
  s->f_bsize = s->f_frsize = 4096;
  s->f_blocks = total_b / 4096;
  s->f_bfree = s->f_bavail = free_b / 4096;
  s->f_files = 100000;
  s->f_ffree = 50000;
  s->f_namelen = 255;
  return 0;
}
