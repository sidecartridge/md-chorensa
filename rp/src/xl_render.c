/**
 * File: xl_render.c
 * Description: The translated game's picture in the ST's 16 colours. See
 *              xl_render.h.
 */

#include "xl_render.h"

#include <string.h>

#include "xl.h"

/* The X68000's registers and planes the picture reads, as the game sees
 * them (tools/xlat/host/xl_host.c draws from the same). */
#define TEXT_VRAM 0xE00000u
#define TEXT_PLANE 0x20000u
#define TEXT_LINE 128u
#define CRTC 0xE80000u
#define CRTC_TEXT_X (CRTC + 0x14u)
#define CRTC_TEXT_Y (CRTC + 0x16u)
#define CRTC_PAGE0_Y (CRTC + 0x1Au)
#define CRTC_PAGE1_Y (CRTC + 0x22u)
#define PALETTES 0xE82000u /* the graphics palette, then the sprite and text one */
#define VC_R1 0xE82500u
#define ENTRIES 512u
#define ENTRY_SPRITE 256u
/* Sprite coordinates count from 16 pixels above and left of the screen. */
#define SPRITE_GUARD 16
#define SPRITE_LIMIT (XL_RENDER_W + 2 * SPRITE_GUARD - 16)
#define BUCKETS 32u

void xl_render_take_sprites(xl_render_frame_t *f, uint32_t table, uint32_t count) {
  if (count > XL_RENDER_SPRITES) count = XL_RENDER_SPRITES;
#ifdef XL_PAGED
  /* A page's words are in the RP's order (xl.h): read as they are. */
  f->sprite_offset = table & XL_PAGE_MASK;
  for (uint32_t p = 0; (p << XL_PAGE_BITS) < f->sprite_offset + 8u * count; p++) {
    f->sprite_pages[p] = xl_at((table & ~XL_PAGE_MASK) + (p << XL_PAGE_BITS));
  }
#else
  for (uint32_t k = 0; k < count; k++) {
    for (uint32_t w = 0; w < 4; w++) f->sprites[k][w] = (uint16_t)rd16(table + 8u * k + 2u * w);
  }
#endif
  f->sprite_count = count;
}

/* Sprite k's four words. */
static inline const uint16_t *sprite_words(const xl_render_frame_t *f, uint32_t k) {
#ifdef XL_PAGED
  uint32_t at = f->sprite_offset + 8u * k;
  return (const uint16_t *)(f->sprite_pages[at >> XL_PAGE_BITS] + (at & XL_PAGE_MASK));
#else
  return f->sprites[k];
#endif
}

void xl_render_prepare(xl_render_frame_t *f) {
  f->page0_y = (uint16_t)rd16(CRTC_PAGE0_Y);
  f->page1_y = (uint16_t)rd16(CRTC_PAGE1_Y);
  f->text_x = (uint16_t)rd16(CRTC_TEXT_X);
  f->text_y = (uint16_t)rd16(CRTC_TEXT_Y);
  uint32_t r1 = rd16(VC_R1);
  /* R1 bits 13-12 the sprites' priority, 11-10 the text's: the sprites
   * drawn first when theirs is the larger. */
  f->text_in_front = ((r1 >> 12) & 3u) > ((r1 >> 10) & 3u);
  /* Buckets by attribute bits 4-0, drawn in ascending order, each bucket in
   * reverse list order (the port's order_sprites). */
  uint16_t ends[BUCKETS];
  uint16_t total = 0;
  memset(ends, 0, sizeof ends);
  for (uint32_t k = 0; k < f->sprite_count; k++) ends[sprite_words(f, k)[3] & 31u]++;
  for (uint32_t b = 0; b < BUCKETS; b++) {
    total = (uint16_t)(total + ends[b]);
    ends[b] = total;
  }
  for (uint32_t k = 0; k < f->sprite_count; k++) {
    f->order[--ends[sprite_words(f, k)[3] & 31u]] = (uint16_t)k;
  }
}

/* A byte of a 4-bit picture as its two pixels, the left first. */
static inline void put_pair(uint8_t *d, uint32_t b) {
  d[0] = (uint8_t)(b >> 4);
  d[1] = (uint8_t)(b & 15u);
}

static void draw_pages(uint8_t *band, const xl_render_frame_t *f, int y0, int y1) {
  const xl_store_art_t *art = f->art;
  uint32_t lines = art->page_lines;
  for (int y = y0; y < y1; y++) {
    uint8_t *d = band + (y - y0) * XL_RENDER_W;
    const uint8_t *src = art->page1 + ((f->page1_y + (uint32_t)y) % lines) * (XL_RENDER_W / 2u);
    for (uint32_t x = 0; x < XL_RENDER_W / 2u; x++) put_pair(d + 2u * x, src[x]);
    const uint8_t *rec = art->page0_runs + art->page0_index[(f->page0_y + (uint32_t)y) % lines];
    uint32_t n = rec[0];
    const uint8_t *pix = rec + 1u + 2u * n;
    for (uint32_t k = 0; k < n; k++) {
      uint32_t x = rec[1u + 2u * k], len = rec[2u + 2u * k] + 1u;
      uint8_t *o = d + x;
      for (uint32_t i = 0; i + 1u < len; i += 2u) put_pair(o + i, pix[i >> 1]);
      if (len & 1u) o[len - 1u] = (uint8_t)(pix[(len - 1u) >> 1] >> 4);
      pix += (len + 1u) >> 1;
    }
  }
}

/* Four planes of 1024 x 1024, colour 0 clear, palette block 0; the
 * horizontal scroll in whole bytes, as the port applies it. */
static void draw_text(uint8_t *band, const xl_render_frame_t *f, int y0, int y1) {
  const uint8_t *colour = f->art->entry_colour + ENTRY_SPRITE;
  uint32_t column = f->text_x >> 3;
  for (int y = y0; y < y1; y++) {
    uint32_t line = TEXT_VRAM + ((f->text_y + (uint32_t)y) & 1023u) * TEXT_LINE;
    uint8_t *d = band + (y - y0) * XL_RENDER_W;
    for (uint32_t b = 0; b < XL_RENDER_W / 8u; b++) {
      uint32_t at = line + ((column + b) & (TEXT_LINE - 1u));
      uint32_t p0 = rd8(at), p1 = rd8(at + TEXT_PLANE), p2 = rd8(at + 2u * TEXT_PLANE),
               p3 = rd8(at + 3u * TEXT_PLANE);
      if ((p0 | p1 | p2 | p3) == 0) continue;
      for (uint32_t k = 0; k < 8; k++) {
        uint32_t bit = 7u - k;
        uint32_t c = (((p3 >> bit) & 1u) << 3) | (((p2 >> bit) & 1u) << 2) |
                     (((p1 >> bit) & 1u) << 1) | ((p0 >> bit) & 1u);
        if (c) d[8u * b + k] = colour[c];
      }
    }
  }
}

/* 16 x 16 at 4 bits a pixel in rows of 8 bytes; attributes bits 11-8 the
 * palette block, 14 a horizontal and 15 a vertical flip. */
static void draw_sprites(uint8_t *band, const xl_render_frame_t *f, int y0, int y1) {
  const xl_store_art_t *art = f->art;
  for (uint32_t k = 0; k < f->sprite_count; k++) {
    const uint16_t *s = sprite_words(f, f->order[k]);
    uint32_t sx = s[0], sy = s[1], pattern = s[2], attr = s[3];
    if (sx > SPRITE_LIMIT || sy > SPRITE_LIMIT || pattern >= art->pattern_count) continue;
    uint32_t at = art->pattern_index[pattern];
    if (at == XL_STORE_NO_PATTERN) continue;
    int x = (int)sx - SPRITE_GUARD, y = (int)sy - SPRITE_GUARD;
    int r0 = y0 > y ? y0 - y : 0, r1 = y1 < y + 16 ? y1 - y : 16;
    if (r0 >= r1 || x <= -16 || x >= XL_RENDER_W) continue;
    const uint8_t *lut = art->entry_colour + ENTRY_SPRITE + ((attr >> 8) & 15u) * 16u;
    const uint8_t *data = (at & XL_STORE_COMMON_PATTERN)
                              ? art->common_patterns + (at & ~XL_STORE_COMMON_PATTERN) * 128u
                              : art->patterns + at * 128u;
    int hflip = (attr & 0x4000u) != 0, vflip = (attr & 0x8000u) != 0;
    for (int r = r0; r < r1; r++) {
      const uint8_t *row = data + (vflip ? 15 - r : r) * 8;
      uint8_t *d = band + (y + r - y0) * XL_RENDER_W;
      for (int c = 0; c < 16; c++) {
        uint32_t v = (c & 1) ? (row[c >> 1] & 15u) : (row[c >> 1] >> 4);
        if (v == 0) continue;
        int px = x + (hflip ? 15 - c : c);
        if (px >= 0 && px < XL_RENDER_W) d[px] = lut[v];
      }
    }
  }
}

void xl_render_band(uint8_t *band, const xl_render_frame_t *f, int y0, int y1) {
  if (y0 < 0 || y1 > XL_RENDER_H || y0 >= y1) return;
  draw_pages(band, f, y0, y1);
  if (f->text_in_front) {
    draw_sprites(band, f, y0, y1);
    draw_text(band, f, y0, y1);
  } else {
    draw_text(band, f, y0, y1);
    draw_sprites(band, f, y0, y1);
  }
}

/* The palette's words the last colours were worked out from, and the art
 * (a scene's) they were worked out with. */
static uint16_t s_seen[ENTRIES];
static int s_valid;
static const xl_store_art_t *s_seen_art;

void xl_render_palette_forget(void) { s_valid = 0; }

int xl_render_palette(const xl_store_art_t *art, uint16_t out[16]) {
  int changed = !s_valid || art != s_seen_art;
  s_seen_art = art;
  for (uint32_t e = 0; e < ENTRIES; e++) {
    uint16_t w = (uint16_t)rd16(PALETTES + 2u * e);
    if (w != s_seen[e]) {
      s_seen[e] = w;
      changed = 1;
    }
  }
  if (!changed) return 0;
  s_valid = 1;
  uint32_t sums[16][4];
  memset(sums, 0, sizeof sums);
  for (uint32_t e = 0; e < ENTRIES; e++) {
    uint32_t weight = art->entry_weight[e];
    if (weight == 0) continue;
    uint32_t word = s_seen[e];
    /* GRB555 (the intensity bit ignored), each component 5 to 8 bits. */
    uint32_t g = (word >> 11) & 31u, r = (word >> 6) & 31u, b = (word >> 1) & 31u;
    uint32_t *acc = sums[art->entry_colour[e] & 15u];
    acc[0] += weight * art->linear[(r << 3) | (r >> 2)];
    acc[1] += weight * art->linear[(g << 3) | (g >> 2)];
    acc[2] += weight * art->linear[(b << 3) | (b >> 2)];
    acc[3] += weight;
  }
  for (uint32_t n = 0; n < 16; n++) {
    uint32_t level[3] = {0, 0, 0};
    if (sums[n][3]) {
      for (uint32_t c = 0; c < 3; c++) {
        uint32_t mean = sums[n][c] / sums[n][3];
        uint32_t k = 0;
        while (k < 7u && mean >= art->thresholds[k]) k++;
        level[c] = k;
      }
    }
    out[n] = (uint16_t)((level[0] << 8) | (level[1] << 4) | level[2]);
  }
  return 1;
}
