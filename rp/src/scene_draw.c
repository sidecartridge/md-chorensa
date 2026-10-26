/**
 * File: scene_draw.c
 * Description: The bench scene's drawing primitives (scene_draw.h). They run
 *              from RAM so the XIP cache serves only the data they read from
 *              flash, and every one clips to the buffer's width and to the
 *              band [y0, y1).
 */

#include "scene_draw.h"

#include <string.h>

#include "pico/stdlib.h"

#pragma GCC optimize("O3")

/* Two pixels of a 4-bit byte as a little-endian halfword: the left pixel
 * (the high nibble) in the low byte, where the buffer has it first. */
static uint16_t s_unpack[256];

void scene_draw_init(void) {
  for (uint32_t b = 0; b < 256u; b++) {
    s_unpack[b] = (uint16_t)((b >> 4) | ((b & 15u) << 8));
  }
}

static inline int clamp_band(int *y0, int *y1) {
  if (*y0 < 0) *y0 = 0;
  if (*y1 > SCENE_W) *y1 = SCENE_W;
  return *y0 < *y1;
}

void __not_in_flash_func(scene_page4)(uint8_t *fb, const uint8_t *page, int lines,
                                      int scroll, int y0, int y1) {
  if (!clamp_band(&y0, &y1)) return;
  for (int y = y0; y < y1; y++) {
    int line = (scroll + y) % lines;
    const uint32_t *s = (const uint32_t *)(page + line * (SCENE_W / 2));
    uint32_t *d = (uint32_t *)(fb + y * SCENE_W);
    for (int i = 0; i < SCENE_W / 8; i++) {
      uint32_t w = s[i];
      d[2 * i] = s_unpack[w & 0xFFu] | ((uint32_t)s_unpack[(w >> 8) & 0xFFu] << 16);
      d[2 * i + 1] =
          s_unpack[(w >> 16) & 0xFFu] | ((uint32_t)s_unpack[w >> 24] << 16);
    }
  }
}

void __not_in_flash_func(scene_page4_key)(uint8_t *fb, const uint8_t *page,
                                          int lines, int scroll, int y0, int y1) {
  if (!clamp_band(&y0, &y1)) return;
  for (int y = y0; y < y1; y++) {
    int line = (scroll + y) % lines;
    const uint8_t *s = page + line * (SCENE_W / 2);
    uint8_t *d = fb + y * SCENE_W;
    for (int i = 0; i < SCENE_W / 2; i++) {
      uint32_t b = s[i];
      if (b == 0u) continue;
      uint32_t hi = b >> 4, lo = b & 15u;
      if (hi) d[2 * i] = (uint8_t)hi;
      if (lo) d[2 * i + 1] = (uint8_t)lo;
    }
  }
}

void __not_in_flash_func(scene_page_runs)(uint8_t *fb, const uint8_t *runs,
                                          const uint32_t *index, int lines,
                                          int scroll, int y0, int y1) {
  if (!clamp_band(&y0, &y1)) return;
  for (int y = y0; y < y1; y++) {
    const uint8_t *rec = runs + index[(scroll + y) % lines];
    uint32_t n = rec[0];
    const uint8_t *pix = rec + 1 + 2u * n;
    uint8_t *d = fb + y * SCENE_W;
    for (uint32_t k = 0; k < n; k++) {
      uint32_t x = rec[1 + 2u * k];
      uint32_t len = rec[2 + 2u * k] + 1u;
      memcpy(d + x, pix, len);
      pix += len;
    }
  }
}

/* The rows of a sprite at y within the band: [r0, r1) of 0..15. */
static inline int sprite_rows(int y, int y0, int y1, int *r0, int *r1) {
  if (y0 < 0) y0 = 0;
  if (y1 > SCENE_W) y1 = SCENE_W;
  *r0 = y0 > y ? y0 - y : 0;
  *r1 = y1 < y + SCENE_SPRITE ? y1 - y : SCENE_SPRITE;
  return *r0 < *r1;
}

void __not_in_flash_func(scene_sprite4)(uint8_t *fb, const uint8_t *pattern,
                                        const uint8_t *lut, int x, int y, int y0,
                                        int y1) {
  int r0, r1;
  if (x <= -SCENE_SPRITE || x >= SCENE_W || !sprite_rows(y, y0, y1, &r0, &r1)) {
    return;
  }
  const uint8_t *s = pattern + r0 * (SCENE_SPRITE / 2);
  uint8_t *d = fb + (y + r0) * SCENE_W + x;
  if (x >= 0 && x <= SCENE_W - SCENE_SPRITE) {
    for (int r = r0; r < r1; r++, s += SCENE_SPRITE / 2, d += SCENE_W) {
      for (int i = 0; i < SCENE_SPRITE / 2; i++) {
        uint32_t b = s[i];
        if (b == 0u) continue;
        uint32_t hi = b >> 4, lo = b & 15u;
        if (hi) d[2 * i] = lut[hi];
        if (lo) d[2 * i + 1] = lut[lo];
      }
    }
    return;
  }
  /* Across the left or right edge: a pixel at a time. */
  for (int r = r0; r < r1; r++, s += SCENE_SPRITE / 2, d += SCENE_W) {
    for (int c = 0; c < SCENE_SPRITE; c++) {
      int sx = x + c;
      if (sx < 0 || sx >= SCENE_W) continue;
      uint32_t b = s[c >> 1];
      uint32_t v = (c & 1) ? (b & 15u) : (b >> 4);
      if (v) d[c] = lut[v];
    }
  }
}

void __not_in_flash_func(scene_sprite_rle)(uint8_t *fb, const uint8_t *rle, int x,
                                           int y, int y0, int y1) {
  int r0, r1;
  if (x <= -SCENE_SPRITE || x >= SCENE_W || !sprite_rows(y, y0, y1, &r0, &r1)) {
    return;
  }
  const uint8_t *s = rle;
  for (int r = 0; r < r0; r++) { /* skip the rows above the band */
    uint32_t n = *s++;
    for (uint32_t k = 0; k < n; k++) {
      s += 2u + (*s & 15u);
    }
  }
  uint8_t *line = fb + (y + r0) * SCENE_W;
  int inside = x >= 0 && x <= SCENE_W - SCENE_SPRITE;
  for (int r = r0; r < r1; r++, line += SCENE_W) {
    uint32_t n = *s++;
    for (uint32_t k = 0; k < n; k++) {
      uint32_t head = *s++;
      int start = x + (int)(head >> 4);
      uint32_t len = (head & 15u) + 1u;
      if (inside) {
        uint8_t *d = line + start;
        for (uint32_t i = 0; i < len; i++) d[i] = s[i];
      } else {
        for (uint32_t i = 0; i < len; i++) {
          int sx = start + (int)i;
          if (sx >= 0 && sx < SCENE_W) line[sx] = s[i];
        }
      }
      s += len;
    }
  }
}
