#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bionic.h"
#include "util/log.h"

/* ---------- time ---------- */

int b_clock_gettime(int clk, struct bionic_timespec *ts) {
  int64_t us;
  switch (clk) {
  case 0:  /* CLOCK_REALTIME */
  case 8:  /* CLOCK_REALTIME_ALARM */
    us = bionic_realtime_us();
    break;
  case 2:  /* CLOCK_PROCESS_CPUTIME_ID */
  case 3:  /* CLOCK_THREAD_CPUTIME_ID */
  default: /* MONOTONIC(1), MONOTONIC_RAW(4), BOOTTIME(7), *_COARSE */
    us = bionic_monotonic_us();
    break;
  }
  ts->tv_sec = (int32_t)(us / 1000000);
  ts->tv_nsec = (int32_t)(us % 1000000) * 1000;
  return 0;
}

int b_clock_getres(int clk, struct bionic_timespec *ts) {
  if (ts) {
    ts->tv_sec = 0;
    ts->tv_nsec = 1000;
  }
  return 0;
}

int b_gettimeofday(struct bionic_timeval *tv, void *tz) {
  if (tv) {
    int64_t us = bionic_realtime_us();
    tv->tv_sec = (int32_t)(us / 1000000);
    tv->tv_usec = (int32_t)(us % 1000000);
  }
  return 0;
}

int32_t b_time(int32_t *t) {
  int32_t s = (int32_t)(bionic_realtime_us() / 1000000);
  if (t)
    *t = s;
  return s;
}

int b_nanosleep(const struct bionic_timespec *req, struct bionic_timespec *rem) {
  int64_t us = (int64_t)req->tv_sec * 1000000 + req->tv_nsec / 1000;
  sceKernelDelayThread(us > 0 ? (SceUInt)us : 0);
  if (rem)
    rem->tv_sec = rem->tv_nsec = 0;
  return 0;
}

int b_usleep(unsigned us) {
  sceKernelDelayThread(us);
  return 0;
}

unsigned b_sleep(unsigned s) {
  sceKernelDelayThread(s * 1000000);
  return 0;
}

long b_clock(void) {
  return (long)bionic_monotonic_us(); /* CLOCKS_PER_SEC = 1000000 on bionic */
}

/* ---------- system info ---------- */

long b_sysconf(int name) {
  switch (name) {
  case 0x27: /* _SC_PAGESIZE */
  case 0x28: /* _SC_PAGE_SIZE */
    return 4096;
  case 0x60: /* _SC_NPROCESSORS_CONF */
  case 0x61: /* _SC_NPROCESSORS_ONLN */
    return 3;
  case 0x62: /* _SC_PHYS_PAGES */
    return (512 * 1024 * 1024) / 4096;
  case 0x63: /* _SC_AVPHYS_PAGES */
    return (256 * 1024 * 1024) / 4096;
  case 6: /* _SC_CLK_TCK */
    return 100;
  default:
    LOG("[misc] sysconf(0x%X) unknown", name);
    bionic_set_errno(22);
    return -1;
  }
}

struct bionic_utsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

int b_uname(struct bionic_utsname *u) {
  memset(u, 0, sizeof(*u));
  strcpy(u->sysname, "Linux");
  strcpy(u->nodename, "localhost");
  strcpy(u->release, "3.4.0");
  strcpy(u->version, "#1 SMP PREEMPT");
  strcpy(u->machine, "armv7l");
  return 0;
}

int b___system_property_get(const char *name, char *value) {
  static const struct {
    const char *k, *v;
  } props[] = {
    { "ro.product.model", "PS Vita" },
    { "ro.product.manufacturer", "Sony" },
    { "ro.product.brand", "Sony" },
    { "ro.product.device", "vita" },
    { "ro.product.name", "vita" },
    { "ro.build.version.sdk", "19" },
    { "ro.build.version.release", "4.4.4" },
    { "ro.product.cpu.abi", "armeabi-v7a" },
    { "ro.product.cpu.abi2", "armeabi" },
    { "ro.hardware", "vita" },
    { "ro.board.platform", "vita" },
    { "ro.kernel.qemu", "" },
    { "ro.debuggable", "0" },
  };
  for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
    if (strcmp(name, props[i].k) == 0) {
      strcpy(value, props[i].v);
      return strlen(value);
    }
  }
  DLOG("[misc] property %s unset", name);
  value[0] = 0;
  return 0;
}

int b_getpid(void) { return 1000; }
int b_getuid(void) { return 10000; }

struct bionic_passwd {
  char *pw_name, *pw_passwd;
  int pw_uid, pw_gid;
  char *pw_dir, *pw_shell;
};

void *b_getpwuid(int uid) {
  static struct bionic_passwd pw = { "u0_a0", "", 10000, 10000, "/data/data/com.pixel.gun3d", "/system/bin/sh" };
  return &pw;
}

int b_gethostname(char *name, size_t len) {
  snprintf(name, len, "localhost");
  return 0;
}

/* thread names set with PR_SET_NAME, for debug sampling */
static struct {
  char name[32];
  SceUID uid;
} named[64];
static int nnamed;

SceUID thread_by_name(const char *name) {
  for (int i = 0; i < nnamed; i++)
    if (strcmp(named[i].name, name) == 0)
      return named[i].uid;
  return -1;
}

const char *thread_name(SceUID uid) {
  for (int i = 0; i < nnamed; i++)
    if (named[i].uid == uid)
      return named[i].name;
  return NULL;
}

int b_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5) {
  if (option == 15) { /* PR_SET_NAME */
    DLOG("[misc] prctl PR_SET_NAME %s", (const char *)a2);
    int i = __atomic_fetch_add(&nnamed, 1, __ATOMIC_SEQ_CST);
    if (i < 64) {
      strncpy(named[i].name, (const char *)a2, sizeof(named[i].name) - 1);
      named[i].uid = sceKernelGetThreadId();
    } else {
      nnamed = 64;
    }
  }
  return 0;
}

long b_syscall(long n, long a, long b, long c) {
  if (n == 224) /* __NR_gettid */
    return sceKernelGetThreadId();
  if (n == 241) /* sched_setaffinity: the scheduler places threads itself */
    return 0;
  if (n == 242) { /* sched_getaffinity(pid, size, mask): cores 0-2 */
    if (b >= 4)
      *(uint32_t *)c = 0x7;
    return 4;
  }
  LOG("[misc] syscall(%ld, 0x%lX, 0x%lX, 0x%lX) unsupported lr=%p", n, a, b, c, __builtin_return_address(0));
  bionic_set_errno(38);
  return -1;
}

int b_fail_enosys(void) {
  LOG("[misc] unsupported process call lr=%p", __builtin_return_address(0));
  bionic_set_errno(38);
  return -1;
}

char *b_setlocale(int cat, const char *loc) {
  static char c[] = "C";
  return c;
}

char *b_realpath(const char *path, char *resolved) {
  if (!resolved)
    resolved = malloc(512);
  strncpy(resolved, path, 511);
  resolved[511] = 0;
  return resolved;
}

/* ---------- stack protector, atexit ---------- */

uintptr_t b___stack_chk_guard = 0x5a3c9e17;

void b___stack_chk_fail(void) {
  fatal("[misc] stack smashing detected, lr=%p", __builtin_return_address(0));
}

int b___cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
  return 0; /* the process never unloads modules */
}

int b___aeabi_atexit(void *arg, void (*fn)(void *), void *dso) {
  return 0;
}

void b___cxa_finalize(void *dso) {
}

void b_abort(void) {
  fatal("[misc] abort() called from %p", __builtin_return_address(0));
}

void b_exit(int code) {
  fatal("[misc] exit(%d) called from %p", code, __builtin_return_address(0));
}

/* AEABI memory helpers: note __aeabi_memset takes (dest, n, c) */
void b___aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void b___aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void b___aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }

int b___isfinitef(float f) { return __builtin_isfinite(f); }
void b_noop(void) {}

int *b___errno(void) { return &errno; }

/* ---------- android log ---------- */

static const char prio_ch[] = "??VDIWEFS";

int b___android_log_write(int prio, const char *tag, const char *msg) {
  log_printf("[%c/%s] %s", prio_ch[prio & 7], tag ? tag : "", msg ? msg : "");
  return 1;
}

int b___android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  return b___android_log_write(prio, tag, buf);
}

int b___android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = b___android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

/* ---------- bionic SHA1 (SHA1_CTX: state[5], count[2], buffer[64]) ---------- */

typedef struct {
  uint32_t state[5];
  uint32_t count[2];
  uint8_t buffer[64];
} bsha1_ctx;

#define ROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))

static void sha1_transform(uint32_t st[5], const uint8_t blk[64]) {
  uint32_t w[80], a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)blk[i * 4] << 24 | blk[i * 4 + 1] << 16 | blk[i * 4 + 2] << 8 | blk[i * 4 + 3];
  for (int i = 16; i < 80; i++)
    w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
    else { f = b ^ c ^ d; k = 0xca62c1d6; }
    uint32_t t = ROL(a, 5) + f + e + k + w[i];
    e = d; d = c; c = ROL(b, 30); b = a; a = t;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

void b_SHA1Init(bsha1_ctx *c) {
  c->state[0] = 0x67452301; c->state[1] = 0xefcdab89; c->state[2] = 0x98badcfe;
  c->state[3] = 0x10325476; c->state[4] = 0xc3d2e1f0;
  c->count[0] = c->count[1] = 0;
}

void b_SHA1Update(bsha1_ctx *c, const uint8_t *data, uint32_t len) {
  uint32_t j = (c->count[0] >> 3) & 63;
  if ((c->count[0] += len << 3) < (len << 3))
    c->count[1]++;
  c->count[1] += len >> 29;
  uint32_t i = 0;
  if (j + len > 63) {
    i = 64 - j;
    memcpy(&c->buffer[j], data, i);
    sha1_transform(c->state, c->buffer);
    for (; i + 63 < len; i += 64)
      sha1_transform(c->state, &data[i]);
    j = 0;
  }
  memcpy(&c->buffer[j], &data[i], len - i);
}

void b_SHA1Final(uint8_t digest[20], bsha1_ctx *c) {
  uint8_t fc[8];
  for (int i = 0; i < 8; i++)
    fc[i] = (uint8_t)(c->count[i >= 4 ? 0 : 1] >> ((3 - (i & 3)) * 8));
  uint8_t p = 0x80;
  b_SHA1Update(c, &p, 1);
  p = 0;
  while ((c->count[0] & 504) != 448)
    b_SHA1Update(c, &p, 1);
  b_SHA1Update(c, fc, 8);
  for (int i = 0; i < 20; i++)
    digest[i] = (uint8_t)(c->state[i >> 2] >> ((3 - (i & 3)) * 8));
}
