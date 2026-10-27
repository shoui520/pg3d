#include "util/log.h"
#include "util/perf.h"

#include <stdio.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

static SceUID log_fd = -1;
static SceKernelLwMutexWork log_lock;
static int log_ready;

#define LOG_KEEP 5 /* previous runs kept as <path>.1 (newest) .. <path>.5 */

/* A crash is usually followed by a relaunch; keep the runs before it */
static void rotate(const char *path) {
  char from[256], to[256];
  for (int i = LOG_KEEP; i >= 1; i--) {
    sceClibSnprintf(to, sizeof(to), "%s.%d", path, i);
    if (i > 1)
      sceClibSnprintf(from, sizeof(from), "%s.%d", path, i - 1);
    else
      sceClibSnprintf(from, sizeof(from), "%s", path);
    sceIoRemove(to);
    sceIoRename(from, to);
  }
}

void log_init(const char *path) {
  sceKernelCreateLwMutex(&log_lock, "pg3d_log", 2, 0, NULL);
  log_ready = 1;
  if (path) {
    rotate(path);
    log_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  }
}

void log_vprintf(const char *fmt, va_list ap) {
  char buf[1024];
  int n = sceClibVsnprintf(buf, sizeof(buf), fmt, ap);
  if (n < 0)
    return;
  if (n >= (int)sizeof(buf))
    n = sizeof(buf) - 1;
  if (n == 0 || buf[n - 1] != '\n') {
    if (n < (int)sizeof(buf) - 1)
      buf[n++] = '\n';
    else
      buf[n - 1] = '\n';
    buf[n] = 0;
  }
  uint32_t t = perf_now();
  /* a writer stuck in sceIoWrite must not take the watchdog down with it:
   * after a while, print to the tty only */
  SceUInt timeout = 2 * 1000 * 1000;
  int locked = log_ready && sceKernelLockLwMutex(&log_lock, 1, &timeout) >= 0;
  sceClibPrintf("%s", buf);
  if (log_fd >= 0 && (locked || !log_ready))
    sceIoWrite(log_fd, buf, n);
  if (locked)
    sceKernelUnlockLwMutex(&log_lock, 1);
  perf_add(PERF_LOG, t, n);
}

void log_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_vprintf(fmt, ap);
  va_end(ap);
}

void log_flush(void) {
  if (log_fd >= 0)
    sceIoSyncByFd(log_fd, 0);
}

void fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_vprintf(fmt, ap);
  va_end(ap);
  log_flush();
  __builtin_trap();
}
