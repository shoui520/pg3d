/*
 * libOpenSLES (vitasdk) links libsndfile for URI/file data sources. Unity's
 * FMOD only feeds buffer queues, so file decoding is not provided: sf_open
 * fails and the URI player reports SL_RESULT_CONTENT_UNSUPPORTED.
 */
#include <stddef.h>
#include <stdint.h>

#include "util/log.h"

void *sf_open(const char *path, int mode, void *info) {
  LOG("[sles] file source %s not supported", path ? path : "(null)");
  return NULL;
}

int sf_close(void *f) { return 0; }
int64_t sf_seek(void *f, int64_t frames, int whence) { return -1; }
int64_t sf_read_short(void *f, short *ptr, int64_t items) { return 0; }
