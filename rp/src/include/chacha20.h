#ifndef CHACHA20_H
#define CHACHA20_H

#include <stdint.h>

/* ChaCha20 (RFC 8439): a 256-bit key, a 96-bit nonce, a 32-bit block
 * counter from 0; chacha20_xor encrypts and decrypts alike, in pieces of
 * any size. Plain C, no SDK. */
typedef struct {
  uint32_t input[16];
  uint8_t keystream[64];
  uint32_t used; /* bytes of keystream used, 64: none left */
} chacha20_t;

void chacha20_start(chacha20_t *c, const uint8_t key[32], const uint8_t nonce[12]);
void chacha20_xor(chacha20_t *c, uint8_t *data, uint32_t length);

#endif
