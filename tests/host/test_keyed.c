// units: rp/src/sha256.c rp/src/chacha20.c
/* The keyed images' primitives against published vectors: SHA-256 of "abc"
 * and of the empty string (FIPS 180-4's examples), whole and in pieces;
 * ChaCha20's keystream for the all-zero key and nonce (RFC 8439's first
 * test, as OpenSSL gives it), and a round trip in pieces of every size. */
#include <string.h>

#include "chacha20.h"
#include "sha256.h"
#include "test.h"

static int digest_is(const uint8_t d[32], const char *hex) {
  static const char k_hex[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++)
    if (hex[2 * i] != k_hex[d[i] >> 4] || hex[2 * i + 1] != k_hex[d[i] & 15]) return 0;
  return 1;
}

int main(void) {
  sha256_t s;
  uint8_t d[32];
  sha256_start(&s);
  sha256_update(&s, "abc", 3);
  sha256_end(&s, d);
  CHECK(digest_is(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  sha256_start(&s);
  sha256_end(&s, d);
  CHECK(digest_is(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  /* 1,000 bytes in pieces of 1 to 77 equal them whole. */
  static uint8_t data[1000];
  for (int i = 0; i < 1000; i++) data[i] = (uint8_t)(i * 7 + 3);
  uint8_t whole[32];
  sha256_start(&s);
  sha256_update(&s, data, sizeof data);
  sha256_end(&s, whole);
  sha256_start(&s);
  for (uint32_t at = 0, n = 1; at < sizeof data; at += n, n = n % 77u + 1u)
    sha256_update(&s, data + at, at + n <= sizeof data ? n : (uint32_t)sizeof data - at);
  sha256_end(&s, d);
  CHECK(memcmp(d, whole, 32) == 0);

  static const uint8_t zero[32] = {0};
  static const uint8_t k_stream[32] = {0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90,
                                       0x40, 0x5d, 0x6a, 0xe5, 0x53, 0x86, 0xbd, 0x28,
                                       0xbd, 0xd2, 0x19, 0xb8, 0xa0, 0x8d, 0xed, 0x1a,
                                       0xa8, 0x36, 0xef, 0xcc, 0x8b, 0x77, 0x0d, 0xc7};
  chacha20_t c;
  uint8_t block[32] = {0};
  chacha20_start(&c, zero, zero);
  chacha20_xor(&c, block, sizeof block);
  CHECK(memcmp(block, k_stream, 32) == 0);
  uint8_t copy[1000];
  memcpy(copy, data, sizeof data);
  chacha20_start(&c, whole, zero + 4);
  chacha20_xor(&c, copy, sizeof copy);
  CHECK(memcmp(copy, data, sizeof data) != 0);
  chacha20_start(&c, whole, zero + 4);
  for (uint32_t at = 0, n = 1; at < sizeof copy; at += n, n = n % 131u + 1u)
    chacha20_xor(&c, copy + at, at + n <= sizeof copy ? n : (uint32_t)sizeof copy - at);
  CHECK(memcmp(copy, data, sizeof data) == 0);
  TEST_END();
}
