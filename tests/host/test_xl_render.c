// units: rp/src/xl_render.c
// cflags: -I ../../tools/xlat
/* The translated game's renderer (xl_render.c) against a reference: the
 * renderer as it was first written, pixel by pixel, kept here. Random
 * scenes: both graphics pages (page 0 as runs over page 1) scrolled, the
 * text plane scrolled, up to the game's 512 sprites anywhere around and off
 * the screen, with both flips, every palette block, priority buckets out of
 * list order, patterns of the scene's, of COMMON's and missing; text in front
 * of the sprites and behind. Each frame drawn whole and in bands (the RP's
 * 16 lines, and others), against the reference's whole frame. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "xl.h"
#include "xl_render.h"

uint8_t *xl_mem;
#ifdef XL_PAGED
/* The RP's page table over one flat area: every page in place. */
uint8_t **xl_regions[256];
static uint8_t *s_rows[256][256];
#endif

#define LINES 512u
#define PATTERNS 300u
#define COMMON_PATTERNS 200u
#define PATTERN_COUNT 600u

static uint32_t s_seed = 12345u;
static uint32_t rnd(void) {
  s_seed = s_seed * 1103515245u + 12345u;
  return s_seed >> 8;
}


/* --- The reference: the first renderer ------------------------------------- */

#define TEXT_VRAM 0xE00000u
#define TEXT_PLANE 0x20000u
#define TEXT_LINE 128u
#define ENTRY_SPRITE 256u
#define SPRITE_GUARD 16
#define SPRITE_LIMIT (XL_RENDER_W + 2 * SPRITE_GUARD - 16)
#define TABLE 0x100000u /* the sprite list */

static void ref_pair(uint8_t *d, uint32_t b) {
  d[0] = (uint8_t)(b >> 4);
  d[1] = (uint8_t)(b & 15u);
}

static void ref_pages(uint8_t *band, const xl_render_frame_t *f, int y0, int y1) {
  const xl_store_art_t *art = f->art;
  uint32_t lines = art->page_lines;
  for (int y = y0; y < y1; y++) {
    uint8_t *d = band + (y - y0) * XL_RENDER_W;
    const uint8_t *src = art->page1 + ((f->page1_y + (uint32_t)y) % lines) * (XL_RENDER_W / 2u);
    for (uint32_t x = 0; x < XL_RENDER_W / 2u; x++) ref_pair(d + 2u * x, src[x]);
    const uint8_t *rec = art->page0_runs + art->page0_index[(f->page0_y + (uint32_t)y) % lines];
    uint32_t n = rec[0];
    const uint8_t *pix = rec + 1u + 2u * n;
    for (uint32_t k = 0; k < n; k++) {
      uint32_t x = rec[1u + 2u * k], len = rec[2u + 2u * k] + 1u;
      uint8_t *o = d + x;
      for (uint32_t i = 0; i + 1u < len; i += 2u) ref_pair(o + i, pix[i >> 1]);
      if (len & 1u) o[len - 1u] = (uint8_t)(pix[(len - 1u) >> 1] >> 4);
      pix += (len + 1u) >> 1;
    }
  }
}

static void ref_text(uint8_t *band, const xl_render_frame_t *f, int y0, int y1) {
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

static void ref_sprites(uint8_t *band, const xl_render_frame_t *f, int y0, int y1) {
  const xl_store_art_t *art = f->art;
  for (uint32_t k = 0; k < f->sprite_count; k++) {
    uint32_t at0 = TABLE + 8u * f->order[k];
    uint32_t sx = rd16(at0), sy = rd16(at0 + 2u), pattern = rd16(at0 + 4u), attr = rd16(at0 + 6u);
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

static void ref_frame(uint8_t *out, const xl_render_frame_t *f) {
  ref_pages(out, f, 0, XL_RENDER_H);
  if (f->text_in_front) {
    ref_sprites(out, f, 0, XL_RENDER_H);
    ref_text(out, f, 0, XL_RENDER_H);
  } else {
    ref_text(out, f, 0, XL_RENDER_H);
    ref_sprites(out, f, 0, XL_RENDER_H);
  }
}

/* --- A random scene --------------------------------------------------------- */

static uint8_t s_page1[LINES * XL_RENDER_W / 2] __attribute__((aligned(4)));
static uint8_t s_runs[LINES * 400];
static uint32_t s_index[LINES];
static uint8_t s_patterns[PATTERNS * 128];
static uint8_t s_common[COMMON_PATTERNS * 128];
static uint16_t s_pattern_index[PATTERN_COUNT];
static uint8_t s_colour[512];
static xl_store_art_t s_art;

/* Page 0's line: runs of opaque pixels, left to right, apart. */
static uint32_t make_runs(uint8_t *rec) {
  uint32_t n = 0, x = rnd() % 40u;
  uint32_t pairs[16][2];
  while (n < 16u && x < XL_RENDER_W) {
    uint32_t len = 1u + rnd() % 60u;
    if (x + len > XL_RENDER_W) len = XL_RENDER_W - x;
    pairs[n][0] = x;
    pairs[n][1] = len;
    n++;
    x += len + 1u + rnd() % 50u;
  }
  rec[0] = (uint8_t)n;
  uint32_t at = 1u + 2u * n;
  for (uint32_t k = 0; k < n; k++) {
    rec[1u + 2u * k] = (uint8_t)pairs[k][0];
    rec[2u + 2u * k] = (uint8_t)(pairs[k][1] - 1u);
    for (uint32_t b = 0; b < (pairs[k][1] + 1u) / 2u; b++) rec[at++] = (uint8_t)rnd();
  }
  return at;
}

/* A pattern: mostly opaque, some clear rows, clear halves and clear pixels. */
static void make_pattern(uint8_t *p) {
  for (uint32_t r = 0; r < 16; r++) {
    uint32_t kind = rnd() % 8u;
    for (uint32_t b = 0; b < 8; b++) {
      uint8_t v = (uint8_t)rnd();
      if (kind == 0) v = 0;
      if (kind == 1 && b < 4) v = 0;
      if (kind == 2 && b >= 4) v = 0;
      if (kind == 3 && (rnd() & 1u)) v &= 0x0F;
      if (kind == 4 && (rnd() & 1u)) v &= 0xF0;
      p[r * 8 + b] = v;
    }
  }
}

static void make_art(void) {
  for (uint32_t k = 0; k < sizeof s_page1; k++) s_page1[k] = (uint8_t)rnd();
  uint32_t at = 0;
  for (uint32_t y = 0; y < LINES; y++) {
    s_index[y] = at;
    at += make_runs(s_runs + at);
  }
  for (uint32_t k = 0; k < PATTERNS; k++) make_pattern(s_patterns + 128 * k);
  for (uint32_t k = 0; k < COMMON_PATTERNS; k++) make_pattern(s_common + 128 * k);
  for (uint32_t k = 0; k < PATTERN_COUNT; k++) {
    uint32_t r = rnd() % 10u;
    s_pattern_index[k] = r < 5u   ? (uint16_t)(rnd() % PATTERNS)
                         : r < 9u ? (uint16_t)(XL_STORE_COMMON_PATTERN | (rnd() % COMMON_PATTERNS))
                                  : (uint16_t)XL_STORE_NO_PATTERN;
  }
  for (uint32_t k = 0; k < 512; k++) s_colour[k] = (uint8_t)(rnd() & 15u);
  s_art.page1 = s_page1;
  s_art.page0_runs = s_runs;
  s_art.page0_index = s_index;
  s_art.page_lines = LINES;
  s_art.patterns = s_patterns;
  s_art.common_patterns = s_common;
  s_art.pattern_index = s_pattern_index;
  s_art.pattern_count = PATTERN_COUNT - 50u; /* the last ones out of range */
  s_art.entry_colour = s_colour;
}

/* The text plane: empty, sparse or busy lines, in its four planes. */
static void make_text(void) {
  memset(xl_mem + TEXT_VRAM, 0, 4u * TEXT_PLANE); /* clear in either byte order */
  for (uint32_t line = 0; line < 1024; line++) {
    uint32_t kind = rnd() % 4u;
    if (kind == 0) continue;
    for (uint32_t b = 0; b < TEXT_LINE; b++) {
      if (kind == 1 && rnd() % 8u) continue;
      for (uint32_t p = 0; p < 4; p++) wr8(TEXT_VRAM + p * TEXT_PLANE + line * TEXT_LINE + b, rnd());
    }
  }
}

/* A sprite's coordinate: on the screen, across an edge, off it, or past the
 * limit the renderer drops. */
static uint32_t coordinate(void) {
  uint32_t r = rnd() % 10u;
  if (r < 6u) return SPRITE_GUARD + rnd() % (XL_RENDER_W - 16u); /* wholly on */
  if (r < 8u) return rnd() % 32u + (rnd() & 1u ? XL_RENDER_W : 0u); /* across an edge */
  if (r < 9u) return rnd() % (SPRITE_LIMIT + 1u);
  return SPRITE_LIMIT + 1u + rnd() % 2000u;
}

static uint32_t make_scene(xl_render_frame_t *f, uint32_t count) {
  for (uint32_t k = 0; k < count; k++) {
    uint32_t attr = rnd() & 0xCF1Fu; /* flips, palette block, bucket */
    wr16(TABLE + 8u * k, coordinate());
    wr16(TABLE + 8u * k + 2u, coordinate());
    wr16(TABLE + 8u * k + 4u, rnd() % PATTERN_COUNT);
    wr16(TABLE + 8u * k + 6u, attr);
  }
  wr16(0xE8001Au, rnd() % 1024u); /* page 0's scroll */
  wr16(0xE80022u, rnd() % 1024u); /* page 1's */
  /* The text's: unscrolled as the game shows it, or anywhere. */
  wr16(0xE80014u, rnd() & 1u ? 0u : rnd() % 1024u);
  wr16(0xE80016u, rnd() % 1024u);
  wr16(0xE82500u, rnd() & 0x3F00u); /* R1: the priorities */
  memset(f, 0, sizeof *f);
  f->art = &s_art;
  xl_render_take_sprites(f, TABLE, count);
  xl_render_prepare(f);
  return count;
}

static uint8_t s_want[XL_RENDER_W * XL_RENDER_H], s_got[XL_RENDER_W * XL_RENDER_H];

static uint32_t first_difference(void) {
  for (uint32_t k = 0; k < sizeof s_got; k++) {
    if (s_got[k] != s_want[k]) return k;
  }
  return 0xFFFFFFFFu;
}

/* With `in_band`, each band of an even height gets page 1's lines in its own
 * second half first, as the RP streams them there (xl_render_band_page1). */
static int check_frame(const xl_render_frame_t *f, int band, int in_band, const char *what) {
  memset(s_got, 0xEE, sizeof s_got);
  for (int y0 = 0; y0 < XL_RENDER_H; y0 += band) {
    int y1 = y0 + band < XL_RENDER_H ? y0 + band : XL_RENDER_H;
    uint8_t *d = s_got + y0 * XL_RENDER_W;
    if (in_band && (y1 - y0) % 2 == 0) {
      uint8_t *lines = d + (y1 - y0) / 2 * XL_RENDER_W;
      for (int y = y0; y < y1; y++) {
        memcpy(lines + (y - y0) * (XL_RENDER_W / 2),
               s_page1 + ((f->page1_y + (uint32_t)y) % LINES) * (XL_RENDER_W / 2), XL_RENDER_W / 2);
      }
      xl_render_band_page1(d, f, y0, y1, lines);
    } else {
      xl_render_band(d, f, y0, y1);
    }
  }
  uint32_t at = first_difference();
  if (at != 0xFFFFFFFFu) {
    printf("  %s, bands of %d%s: pixel (%u, %u) is %u, the reference's %u\n", what, band,
           in_band ? ", page 1 in the band" : "", at % XL_RENDER_W, at / XL_RENDER_W, s_got[at],
           s_want[at]);
    return 1;
  }
  return 0;
}

int main(void) {
  xl_mem = calloc(XL_MEMORY, 1);
  CHECK(xl_mem != NULL);
#ifdef XL_PAGED
  for (uint32_t r = 0; r < 256; r++) {
    for (uint32_t p = 0; p < 256; p++) s_rows[r][p] = xl_mem + (r << 16) + (p << 8);
    xl_regions[r] = s_rows[r];
  }
#endif
  make_art();
  static const uint32_t counts[] = {0, 1, 5, 40, 120, 268, 400, 512, 600};
  static const int bands[] = {16, 256, 1, 7, 100};
  static xl_render_frame_t f;
  int failures = 0;
  for (uint32_t scene = 0; scene < 40; scene++) {
    make_text();
    uint32_t count = counts[scene % (sizeof counts / sizeof counts[0])];
    make_scene(&f, count);
    ref_frame(s_want, &f);
    char what[64];
    snprintf(what, sizeof what, "scene %u (%u sprites, text %s)", scene, count,
             f.text_in_front ? "in front" : "behind");
    for (uint32_t b = 0; b < sizeof bands / sizeof bands[0]; b++) {
      failures += check_frame(&f, bands[b], 0, what);
      failures += check_frame(&f, bands[b], 1, what);
    }
  }
  CHECK_EQ(failures, 0);
  free(xl_mem);
  TEST_END();
}
