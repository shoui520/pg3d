#include "game/gamebin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "util/log.h"
#include "util/sha256.h"

so_module *gamebin_unity;
so_module *gamebin_il2cpp;

static const struct {
  const char *what;
  unsigned size;
  const char *sha256;
} expected[2] = {
  { "libunity.so", 16787536, "98aa0892302856f6c5cc04e45a65f6e4931f38f8fb37fee31d2797a5d4bd85bd" },
  { "libil2cpp.so", 35556492, "e0c7c7454b3104a979ae92eb72386e633474ac3a0b8bae262abb27c5748bb88c" },
};

static int hash_file(const char *path, char hex[65], unsigned *size_out) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return fd;
  sha256_ctx ctx;
  sha256_init(&ctx);
  const size_t chunk = 1024 * 1024;
  uint8_t *buf = malloc(chunk);
  unsigned total = 0;
  int r;
  while ((r = sceIoRead(fd, buf, chunk)) > 0) {
    sha256_update(&ctx, buf, r);
    total += r;
  }
  free(buf);
  sceIoClose(fd);
  uint8_t digest[32];
  sha256_final(&ctx, digest);
  for (int i = 0; i < 32; i++)
    sprintf(hex + i * 2, "%02x", digest[i]);
  *size_out = total;
  return 0;
}

static void stamp(const char *path, char *out, size_t n) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  sceIoGetstat(path, &st);
  snprintf(out, n, "%s %u %04d%02d%02d%02d%02d%02d\n", path, (unsigned)st.st_size, st.st_mtime.year, st.st_mtime.month,
           st.st_mtime.day, st.st_mtime.hour, st.st_mtime.minute, st.st_mtime.second);
}

int gamebin_verify(const char *unity_path, const char *il2cpp_path, const char *cache_path) {
  const char *paths[2] = { unity_path, il2cpp_path };
  char stamps[512] = { 0 };
  for (int i = 0; i < 2; i++) {
    char s[256];
    stamp(paths[i], s, sizeof(s));
    strcat(stamps, s);
  }

  FILE *f = fopen(cache_path, "rb");
  if (f) {
    char cached[512] = { 0 };
    fread(cached, 1, sizeof(cached) - 1, f);
    fclose(f);
    if (strcmp(cached, stamps) == 0)
      return 0;
  }

  for (int i = 0; i < 2; i++) {
    char hex[65];
    unsigned size = 0;
    if (hash_file(paths[i], hex, &size) < 0) {
      LOG("[gamebin] cannot read %s", paths[i]);
      return -1;
    }
    if (size != expected[i].size || strcmp(hex, expected[i].sha256) != 0) {
      LOG("[gamebin] %s mismatch: size %u sha256 %s (expected %u %s)", expected[i].what, size, hex,
          expected[i].size, expected[i].sha256);
      return -1;
    }
    LOG("[gamebin] %s verified", expected[i].what);
  }

  f = fopen(cache_path, "wb");
  if (f) {
    fwrite(stamps, 1, strlen(stamps), f);
    fclose(f);
  }
  return 0;
}

void *gamebin_trampoline(so_module *m, uintptr_t addr) {
  uint32_t *t = so_arena_alloc(m, 16);
  if (!t)
    return NULL;
  t[0] = ((uint32_t *)addr)[0];
  t[1] = ((uint32_t *)addr)[1];
  t[2] = 0xe51ff004; /* ldr pc, [pc, #-4] */
  t[3] = addr + 8;
  return t;
}
