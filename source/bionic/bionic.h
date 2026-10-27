#ifndef PG3D_BIONIC_H
#define PG3D_BIONIC_H

#include <stddef.h>
#include <stdint.h>

struct bionic_timespec {
  int32_t tv_sec;
  int32_t tv_nsec;
};

struct bionic_timeval {
  int32_t tv_sec;
  int32_t tv_usec;
};

typedef struct {
  const char *name;
  void *addr;
} bionic_symbol;

/* errno */
void bionic_set_errno(int linux_errno);
int bionic_errno_from_newlib(int e);
void bionic_errno_sync(void); /* translate current newlib errno in place */

/* time */
int64_t bionic_realtime_us(void);
int64_t bionic_monotonic_us(void);

/* paths: Android path -> Vita path. Returns dst. */
const char *bionic_map_path(const char *src, char *dst, size_t dst_size);
void bionic_paths_init(const char *data_root);
const char *bionic_data_root(void);

/* symbol tables contributed by each bridge file */
extern const bionic_symbol bionic_libc_symbols[];
extern const size_t bionic_libc_symbols_count;

void bthread_init(void);
void bionic_sysfiles_init(void);

#endif
