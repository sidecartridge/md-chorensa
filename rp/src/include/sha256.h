#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

/* SHA-256 (FIPS 180-4), in pieces. Plain C, no SDK. */
typedef struct {
  uint32_t state[8];
  uint64_t bytes;
  uint8_t block[64];
  uint32_t used;
} sha256_t;

void sha256_start(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, uint32_t length);
void sha256_end(sha256_t *s, uint8_t digest[32]);

#endif
