/**
 * File: fb_workers.c
 * Description: The framebuffer's Thumb assembly workers (fb_chunked_asm.S,
 *              fb_blit_asm.S) as the C they stand for, for builds that cannot
 *              assemble Thumb: EmuMD's host build of the firmware and the host
 *              tests. Same names, same results.
 */
#include <stdint.h>

/* Pixels [src, src_end) to planar words at dst: per 16 pixels, one word per
 * plane (0..3), the leftmost pixel in bit 15. */
void fb_c2p_half(uint16_t *dst, const uint8_t *src, const uint8_t *src_end) {
  for (; src < src_end; src += 16, dst += 4) {
    for (int plane = 0; plane < 4; plane++) {
      uint16_t word = 0;
      for (int i = 0; i < 16; i++) {
        word |= (uint16_t)(((src[i] >> plane) & 1u) << (15 - i));
      }
      dst[plane] = word;
    }
  }
}

/* cw pixels from src to dst, skipping the key colour. */
void fb_blit_key_row(uint8_t *dst, const uint8_t *src, uint32_t cw,
                     uint8_t key) {
  for (uint32_t i = 0; i < cw; i++) {
    if (src[i] != key) dst[i] = src[i];
  }
}
