#ifndef PG3D_PERF_H
#define PG3D_PERF_H

#include <stdint.h>

/* Load-time counters: calls, time and bytes per file/zlib path */
enum perf_kind {
  PERF_FOPEN,
  PERF_FOPEN_FAIL,
  PERF_FREAD,
  PERF_FSEEK,
  PERF_FCLOSE,
  PERF_SYS_OPEN,
  PERF_SYS_READ,
  PERF_SYS_LSEEK,
  PERF_SYS_FSTAT,
  PERF_SYS_CLOSE,
  PERF_INFLATE,
  PERF_ZIP_LOAD,
  PERF_ZIP_HIT,
  PERF_LOG,
  PERF_KINDS
};

/* microseconds, wraps every 71 minutes (only differences are used) */
uint32_t perf_now(void);
void perf_add(enum perf_kind k, uint32_t start, uint32_t bytes);
/* stdio streams opened (+1) or closed (-1) */
void perf_files(int delta);
/* Once per presented frame: logs what the counters gathered when a frame
 * stalled (a scene load blocks the main thread between two frames). */
void perf_frame(void);

#endif
