/*
 * The APK's signing certificate, as android.content.pm.Signature.toByteArray()
 * returns it: the DER of the first certificate in the META-INF .RSA entry (a PKCS#7
 * SignedData). The game hashes it at startup and, on a mismatch, quietly
 * schedules its own shutdown.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "jni/java.h"
#include "util/log.h"

static uint16_t u16(const uint8_t *p) {
  return p[0] | p[1] << 8;
}

static uint32_t u32(const uint8_t *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int ends_with(const char *s, int n, const char *suffix) {
  int l = strlen(suffix);
  return n >= l && !memcmp(s + n - l, suffix, l);
}

/* The signature block entry of a zip, decompressed (malloc'd), or NULL */
static uint8_t *read_signature_block(FILE *f, size_t *out_len) {
  uint8_t tail[0x10000 + 22];
  if (fseek(f, 0, SEEK_END))
    return NULL;
  long size = ftell(f);
  long tail_len = size < (long)sizeof(tail) ? size : (long)sizeof(tail);
  if (fseek(f, size - tail_len, SEEK_SET) || fread(tail, 1, tail_len, f) != (size_t)tail_len)
    return NULL;
  const uint8_t *eocd = NULL;
  for (long i = tail_len - 22; i >= 0 && !eocd; i--)
    if (u32(tail + i) == 0x06054b50)
      eocd = tail + i;
  if (!eocd)
    return NULL;
  uint32_t cd_size = u32(eocd + 12), cd_off = u32(eocd + 16);
  uint8_t *cd = malloc(cd_size);
  if (!cd || fseek(f, cd_off, SEEK_SET) || fread(cd, 1, cd_size, f) != cd_size) {
    free(cd);
    return NULL;
  }
  uint8_t *out = NULL;
  for (uint32_t p = 0; p + 46 <= cd_size && u32(cd + p) == 0x02014b50 && !out;) {
    const uint8_t *e = cd + p;
    int name_len = u16(e + 28);
    const char *name = (const char *)e + 46;
    if (name_len > 9 && !memcmp(name, "META-INF/", 9) &&
        (ends_with(name, name_len, ".RSA") || ends_with(name, name_len, ".DSA") || ends_with(name, name_len, ".EC"))) {
      int method = u16(e + 10);
      uint32_t csize = u32(e + 20), usize = u32(e + 24), loc = u32(e + 42);
      uint8_t lh[30];
      uint8_t *comp = malloc(csize ? csize : 1);
      out = malloc(usize ? usize : 1);
      int ok = comp && out && !fseek(f, loc, SEEK_SET) && fread(lh, 1, 30, f) == 30 && u32(lh) == 0x04034b50 &&
               !fseek(f, loc + 30 + u16(lh + 26) + u16(lh + 28), SEEK_SET) && fread(comp, 1, csize, f) == csize;
      if (ok && method == 0 && csize == usize) {
        memcpy(out, comp, usize);
      } else if (ok && method == 8) {
        z_stream z;
        memset(&z, 0, sizeof(z));
        z.next_in = comp;
        z.avail_in = csize;
        z.next_out = out;
        z.avail_out = usize;
        ok = inflateInit2(&z, -MAX_WBITS) == Z_OK && inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == usize;
        inflateEnd(&z);
      } else {
        ok = 0;
      }
      free(comp);
      if (ok) {
        *out_len = usize;
      } else {
        free(out);
        out = NULL;
      }
    }
    p += 46 + name_len + u16(e + 30) + u16(e + 32);
  }
  free(cd);
  return out;
}

/* One DER element at p: its tag, content and total size; 0 if malformed */
static size_t der(const uint8_t *p, const uint8_t *end, int *tag, const uint8_t **content, size_t *len) {
  if (end - p < 2)
    return 0;
  *tag = p[0];
  size_t n = p[1], hl = 2;
  if (n & 0x80) {
    int k = n & 0x7f;
    if (k < 1 || k > 4 || end - p < 2 + k)
      return 0;
    n = 0;
    for (int i = 0; i < k; i++)
      n = n << 8 | p[2 + i];
    hl += k;
  }
  if ((size_t)(end - p) - hl < n)
    return 0;
  *content = p + hl;
  *len = n;
  return hl + n;
}

/* ContentInfo { OID, [0] SignedData { version, digestAlgorithms, contentInfo, [0] certificates } } */
static int first_certificate(const uint8_t *p, size_t n, const uint8_t **cert, size_t *cert_len) {
  const uint8_t *end = p + n, *c;
  size_t len, sz;
  int tag;
  if (!der(p, end, &tag, &c, &len) || tag != 0x30)
    return 0;
  p = c, end = c + len;
  if (!(sz = der(p, end, &tag, &c, &len)) || tag != 0x06)
    return 0;
  p += sz;
  if (!der(p, end, &tag, &c, &len) || tag != 0xA0)
    return 0;
  p = c, end = c + len;
  if (!der(p, end, &tag, &c, &len) || tag != 0x30)
    return 0;
  p = c, end = c + len;
  for (int i = 0; i < 3; i++) { /* version, digestAlgorithms, contentInfo */
    if (!(sz = der(p, end, &tag, &c, &len)))
      return 0;
    p += sz;
  }
  if (!der(p, end, &tag, &c, &len) || tag != 0xA0)
    return 0;
  if (!(sz = der(c, c + len, &tag, &c, &len)) || tag != 0x30)
    return 0;
  *cert = c - (sz - len); /* the whole element, header included */
  *cert_len = sz;
  return 1;
}

uint8_t *java_apk_certificate(const char *apk_path, size_t *len) {
  FILE *f = fopen(apk_path, "rb");
  if (!f) {
    LOG("[apkcert] can't open %s", apk_path);
    return NULL;
  }
  size_t block_len = 0;
  uint8_t *block = read_signature_block(f, &block_len);
  fclose(f);
  const uint8_t *cert;
  uint8_t *out = NULL;
  if (block && first_certificate(block, block_len, &cert, len) && (out = malloc(*len)))
    memcpy(out, cert, *len);
  free(block);
  LOG("[apkcert] signing certificate: %s (%u bytes)", out ? "found" : "NOT FOUND", out ? (unsigned)*len : 0);
  return out;
}
