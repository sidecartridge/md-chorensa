#include "chacha20.h"

static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define QUARTER(a, b, c, d)                                 \
  a += b, d ^= a, d = rol(d, 16), c += d, b ^= c, b = rol(b, 12), \
  a += b, d ^= a, d = rol(d, 8), c += d, b ^= c, b = rol(b, 7)

static void next_block(chacha20_t *c) {
  uint32_t x[16];
  for (int i = 0; i < 16; i++) x[i] = c->input[i];
  for (int round = 0; round < 10; round++) {
    QUARTER(x[0], x[4], x[8], x[12]);
    QUARTER(x[1], x[5], x[9], x[13]);
    QUARTER(x[2], x[6], x[10], x[14]);
    QUARTER(x[3], x[7], x[11], x[15]);
    QUARTER(x[0], x[5], x[10], x[15]);
    QUARTER(x[1], x[6], x[11], x[12]);
    QUARTER(x[2], x[7], x[8], x[13]);
    QUARTER(x[3], x[4], x[9], x[14]);
  }
  for (int i = 0; i < 16; i++) {
    uint32_t v = x[i] + c->input[i];
    for (int b = 0; b < 4; b++) c->keystream[4 * i + b] = (uint8_t)(v >> (8 * b));
  }
  c->input[12]++;
  c->used = 0;
}

void chacha20_start(chacha20_t *c, const uint8_t key[32], const uint8_t nonce[12]) {
  c->input[0] = 0x61707865u, c->input[1] = 0x3320646eu;
  c->input[2] = 0x79622d32u, c->input[3] = 0x6b206574u;
  for (int i = 0; i < 8; i++) c->input[4 + i] = le32(key + 4 * i);
  c->input[12] = 0;
  for (int i = 0; i < 3; i++) c->input[13 + i] = le32(nonce + 4 * i);
  c->used = 64;
}

void chacha20_xor(chacha20_t *c, uint8_t *data, uint32_t length) {
  for (uint32_t k = 0; k < length; k++) {
    if (c->used == 64u) next_block(c);
    data[k] ^= c->keystream[c->used++];
  }
}
