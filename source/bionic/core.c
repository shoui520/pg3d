/* errno translation, clocks and Android -> Vita path mapping. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <sys/stat.h>

#include "bionic/bionic.h"
#include "util/log.h"

/* ---------- errno ---------- */

int bionic_errno_from_newlib(int e) {
  switch (e) {
  case ENOSYS: return 38;
  case ENOTEMPTY: return 39;
  case ENAMETOOLONG: return 36;
  case ELOOP: return 40;
  case ETIMEDOUT: return 110;
  case EINPROGRESS: return 115;
  case EALREADY: return 114;
  case ECONNREFUSED: return 111;
  case ECONNRESET: return 104;
  case ENOTCONN: return 107;
  case ENOTSOCK: return 88;
  case EADDRINUSE: return 98;
  case ENETUNREACH: return 101;
  case EHOSTUNREACH: return 113;
  case EOVERFLOW: return 75;
  case ENOTSUP: return 95;
  case EDEADLK: return 35;
  case ENOLCK: return 37;
  default: return e < 35 ? e : 5; /* low numbers match Linux; unknown -> EIO */
  }
}

void bionic_set_errno(int linux_errno) {
  errno = linux_errno;
}

void bionic_errno_sync(void) {
  errno = bionic_errno_from_newlib(errno);
}

/* ---------- clocks ---------- */

static int64_t realtime_offset_us;

int64_t bionic_monotonic_us(void) {
  return (int64_t)sceKernelGetProcessTimeWide();
}

int64_t bionic_realtime_us(void) {
  if (!realtime_offset_us) {
    SceRtcTick tick, epoch;
    SceDateTime dt;
    sceRtcGetCurrentTick(&tick);
    memset(&dt, 0, sizeof(dt));
    dt.year = 1970;
    dt.month = 1;
    dt.day = 1;
    sceRtcGetTick(&dt, &epoch);
    realtime_offset_us = (int64_t)(tick.tick - epoch.tick) - bionic_monotonic_us();
  }
  return realtime_offset_us + bionic_monotonic_us();
}

/* ---------- paths ---------- */

static char data_root[128] = "ux0:data/pg3d";

void bionic_paths_init(const char *root) {
  strncpy(data_root, root, sizeof(data_root) - 1);
}

const char *bionic_data_root(void) {
  return data_root;
}

typedef struct {
  const char *android;
  const char *vita; /* relative to data_root */
} path_rule;

static const path_rule rules[] = {
  { "/data/app/com.pixel.gun3d-1/base.apk", "/base.apk" },
  { "/data/app/com.pixel.gun3d-1/lib/arm", "" },
  { "/data/data/com.pixel.gun3d/lib", "" },
  { "/data/data/com.pixel.gun3d/files", "/files" },
  { "/data/data/com.pixel.gun3d/cache", "/cache" },
  { "/data/data/com.pixel.gun3d/shared_prefs", "/shared_prefs" },
  { "/data/data/com.pixel.gun3d", "/files" },
  { "/storage/emulated/0/Android/obb/com.pixel.gun3d", "/obb/com.pixel.gun3d" },
  { "/storage/emulated/0/Android/data/com.pixel.gun3d/files", "/files_ext" },
  { "/storage/emulated/0/Android/data/com.pixel.gun3d/cache", "/cache" },
  { "/storage/emulated/0", "/sdcard" },
  { "/sdcard", "/sdcard" },
};

const char *bionic_map_path(const char *src, char *dst, size_t n) {
  if (!src)
    return NULL;
  if (strchr(src, ':')) { /* already a Vita path */
    snprintf(dst, n, "%s", src);
    return dst;
  }
  for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
    size_t l = strlen(rules[i].android);
    if (strncmp(src, rules[i].android, l) == 0 && (src[l] == 0 || src[l] == '/')) {
      snprintf(dst, n, "%s%s%s", data_root, rules[i].vita, src + l);
      return dst;
    }
  }
  if (src[0] == '/') {
    /* unknown absolute path: keep it under the data root so nothing escapes */
    snprintf(dst, n, "%s/root%s", data_root, src);
    return dst;
  }
  snprintf(dst, n, "%s/files/%s", data_root, src);
  return dst;
}

/* ---------- synthesized /proc and /sys files ---------- */

static void write_if_missing(const char *android_path, const char *content) {
  char vp[256];
  bionic_map_path(android_path, vp, sizeof(vp));
  FILE *f = fopen(vp, "rb");
  if (f) {
    fclose(f);
    return;
  }
  /* create parent directories */
  for (char *s = strchr(vp, '/'); s; s = strchr(s + 1, '/')) {
    *s = 0;
    mkdir(vp, 0777);
    *s = '/';
  }
  f = fopen(vp, "wb");
  if (f) {
    fputs(content, f);
    fclose(f);
  }
}

/* Unity reads CPU count, features and clock from these. The Vita gives
 * applications three Cortex-A9 cores at 444 MHz. */
void bionic_sysfiles_init(void) {
  static const char cpuinfo[] =
    "Processor\t: ARMv7 Processor rev 0 (v7l)\n"
    "processor\t: 0\nBogoMIPS\t: 888.00\n\n"
    "processor\t: 1\nBogoMIPS\t: 888.00\n\n"
    "processor\t: 2\nBogoMIPS\t: 888.00\n\n"
    "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpd32\n"
    "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x4\nCPU part\t: 0xc09\nCPU revision\t: 0\n\n"
    "Hardware\t: PS Vita\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
  write_if_missing("/proc/cpuinfo", cpuinfo);
  write_if_missing("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "444000\n");
  write_if_missing("/sys/devices/system/cpu/possible", "0-2\n");
  write_if_missing("/sys/devices/system/cpu/present", "0-2\n");
  write_if_missing("/proc/uptime", "1000.00 2000.00\n");
  /* placeholders: reads on these fds come from the hardware RNG (io.c) */
  write_if_missing("/dev/urandom", "");
  write_if_missing("/dev/random", "");
}
