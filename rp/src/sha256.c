#include "sha256.h"

#include <string.h>

static const uint32_t k_rounds[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static uint32_t ror(uint32_t v, int n) { return v >> n | v << (32 - n); }

static void block(sha256_t *s, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
           p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3;
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10;
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s->state[0], b = s->state[1], c = s->state[2], d = s->state[3];
  uint32_t e = s->state[4], f = s->state[5], g = s->state[6], h = s->state[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k_rounds[i] + w[i];
    uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  s->state[0] += a, s->state[1] += b, s->state[2] += c, s->state[3] += d;
  s->state[4] += e, s->state[5] += f, s->state[6] += g, s->state[7] += h;
}

void sha256_start(sha256_t *s) {
  static const uint32_t k_start[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  memcpy(s->state, k_start, sizeof k_start);
  s->bytes = 0;
  s->used = 0;
}

void sha256_update(sha256_t *s, const void *data, uint32_t length) {
  const uint8_t *p = data;
  s->bytes += length;
  while (length) {
    uint32_t n = 64u - s->used < length ? 64u - s->used : length;
    memcpy(s->block + s->used, p, n);
    s->used += n;
    p += n;
    length -= n;
    if (s->used == 64u) {
      block(s, s->block);
      s->used = 0;
    }
  }
}

void sha256_end(sha256_t *s, uint8_t digest[32]) {
  uint64_t bits = s->bytes * 8u;
  uint8_t pad = 0x80u;
  sha256_update(s, &pad, 1);
  pad = 0;
  while (s->used != 56u) sha256_update(s, &pad, 1);
  uint8_t length[8];
  for (int i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_update(s, length, 8);
  for (int i = 0; i < 8; i++)
    for (int b = 0; b < 4; b++) digest[4 * i + b] = (uint8_t)(s->state[i] >> (24 - 8 * b));
}
