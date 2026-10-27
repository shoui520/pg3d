/*
 * Load-time counters. File, zlib and log calls add their time and bytes
 * here; scene loads run on the main thread between two frames, so the frame
 * loop reports what piled up over each run of stalled frames.
 */
#include <stdio.h>
#include <psp2/kernel/processmgr.h>

#include "bionic/archive.h"
#include "util/log.h"
#include "util/perf.h"

#define STALL_US 250000  /* a frame this long is loading, not gameplay */
#define QUIET_US 3000000 /* report once frames have been smooth this long */

static const char *const names[PERF_KINDS] = {
  "fopen", "fopen fail", "fread", "fseek", "fclose", "open", "read", "lseek", "fstat", "close", "inflate", "zip load", "zip hit", "log",
};

typedef struct {
  uint32_t calls, us, bytes;
} counter;

static counter total[PERF_KINDS], seen[PERF_KINDS];
static int files, files_peak;

uint32_t perf_now(void) {
  return sceKernelGetProcessTimeLow();
}

void perf_add(enum perf_kind k, uint32_t start, uint32_t bytes) {
  uint32_t us = perf_now() - start;
  __atomic_fetch_add(&total[k].calls, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&total[k].us, us, __ATOMIC_RELAXED);
  if (bytes)
    __atomic_fetch_add(&total[k].bytes, bytes, __ATOMIC_RELAXED);
}

void perf_files(int delta) {
  int now = __atomic_add_fetch(&files, delta, __ATOMIC_RELAXED);
  if (now > files_peak)
    files_peak = now;
}

static void catch_up(void) {
  for (int k = 0; k < PERF_KINDS; k++)
    seen[k] = total[k];
}

static void report(uint32_t window, uint32_t stalled, uint32_t longest, unsigned stalls) {
  char line[1000];
  int n = snprintf(line, sizeof(line), "[perf] %u ms: %u stalled frames, %u ms (longest %u ms), files open %d (peak %d)",
                   window / 1000, stalls, stalled / 1000, longest / 1000, files, files_peak);
  for (int k = 0; k < PERF_KINDS && n < (int)sizeof(line); k++) {
    counter d = { total[k].calls - seen[k].calls, total[k].us - seen[k].us, total[k].bytes - seen[k].bytes };
    if (!d.calls)
      continue;
    n += snprintf(line + n, sizeof(line) - n, ", %s %u", names[k], d.calls);
    if (d.bytes && n < (int)sizeof(line))
      n += snprintf(line + n, sizeof(line) - n, " %u KB", d.bytes / 1024);
    if (n < (int)sizeof(line))
      n += snprintf(line + n, sizeof(line) - n, " %u ms", d.us / 1000);
  }
  LOG("%s", line);
  catch_up();
  archive_after_load();
}

void perf_frame(void) {
  /* the first frame's gap is the boot, from process start */
  static uint32_t prev, start, stalled, longest, last_stall;
  static unsigned stalls;
  uint32_t t = perf_now(), gap = t - prev;
  prev = t;
  if (gap >= STALL_US) {
    stalls++;
    stalled += gap;
    if (gap > longest)
      longest = gap;
    last_stall = t;
  }
  if (!stalls) {
    if (t - start >= QUIET_US) {
      start = t;
      catch_up();
    }
    return;
  }
  if (t - last_stall < QUIET_US)
    return;
  report(t - start, stalled, longest, stalls);
  start = t;
  stalled = longest = stalls = 0;
}
