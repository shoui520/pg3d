/* Straightforward FIPS 180-4 SHA-256. */
#include "util/sha256.h"

#include <string.h>

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void transform(sha256_ctx *c, const uint8_t *d) {
  uint32_t m[64], a, b, cc, dd, e, f, g, h;
  for (int i = 0; i < 16; i++)
    m[i] = (uint32_t)d[i * 4] << 24 | (uint32_t)d[i * 4 + 1] << 16 | (uint32_t)d[i * 4 + 2] << 8 | d[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(m[i - 15], 7) ^ ROR(m[i - 15], 18) ^ (m[i - 15] >> 3);
    uint32_t s1 = ROR(m[i - 2], 17) ^ ROR(m[i - 2], 19) ^ (m[i - 2] >> 10);
    m[i] = m[i - 16] + s0 + m[i - 7] + s1;
  }
  a = c->state[0]; b = c->state[1]; cc = c->state[2]; dd = c->state[3];
  e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + m[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
    h = g; g = f; f = e; e = dd + t1; dd = cc; cc = b; b = a; a = t1 + t2;
  }
  c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += dd;
  c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void sha256_init(sha256_ctx *c) {
  static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  memcpy(c->state, iv, sizeof(iv));
  c->bitlen = 0;
  c->datalen = 0;
}

void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len) {
  while (len) {
    if (c->datalen == 0 && len >= 64) {
      transform(c, data);
      c->bitlen += 512;
      data += 64;
      len -= 64;
      continue;
    }
    size_t n = 64 - c->datalen;
    if (n > len)
      n = len;
    memcpy(c->data + c->datalen, data, n);
    c->datalen += n;
    data += n;
    len -= n;
    if (c->datalen == 64) {
      transform(c, c->data);
      c->bitlen += 512;
      c->datalen = 0;
    }
  }
}

void sha256_final(sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->bitlen + (uint64_t)c->datalen * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  pad = 0;
  while (c->datalen != 56)
    sha256_update(c, &pad, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; i++)
    len[i] = bits >> (56 - i * 8);
  sha256_update(c, len, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = c->state[i] >> 24;
    out[i * 4 + 1] = c->state[i] >> 16;
    out[i * 4 + 2] = c->state[i] >> 8;
    out[i * 4 + 3] = c->state[i];
  }
}
