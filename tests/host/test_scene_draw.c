// units: rp/src/scene_draw.c
/* The bench scene's primitives (scene_draw.c) against a plain reference that
 * draws a pixel at a time from the same data: pages scrolled to every
 * offset, with and without transparency; sprites in all three formats at
 * random positions across every edge, in random bands, as two cores split a
 * frame. Every byte of the buffer must match the reference's. */
#include <stdlib.h>
#include <string.h>

#include "scene_draw.h"
#include "test.h"

#define LINES 40
#define W SCENE_W

static uint8_t page4[LINES * W / 2];
static uint8_t page_px[LINES][W]; /* the same page, a byte a pixel */
static uint8_t runs[LINES * (1 + 2 * 16 + W)];
static uint32_t index_[LINES];

static uint8_t sprite_px[SCENE_SPRITE][SCENE_SPRITE];
static uint8_t sprite4[SCENE_SPRITE * SCENE_SPRITE / 2];
static uint8_t sprite_rle[SCENE_SPRITE * (1 + 2 * SCENE_SPRITE)];

static uint8_t fb[W * W], ref[W * W];

static uint32_t rng = 12345u;
static uint32_t next(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

/* A page: runs of colour and of 0 (transparent), a few per line. */
static void make_page(void) {
  uint8_t *out = runs;
  for (int y = 0; y < LINES; y++) {
    int x = 0;
    while (x < W) {
      int len = 1 + (int)(next() % 40);
      uint8_t colour = (next() % 3) ? 0 : (uint8_t)(1 + next() % 15);
      for (int i = 0; i < len && x < W; i++) page_px[y][x++] = colour;
    }
    for (int x2 = 0; x2 < W; x2 += 2) {
      page4[y * W / 2 + x2 / 2] = (uint8_t)(page_px[y][x2] << 4 | page_px[y][x2 + 1]);
    }
    index_[y] = (uint32_t)(out - runs);
    uint8_t *count = out++;
    *count = 0;
    uint8_t *heads = out;
    int n = 0;
    for (int x2 = 0; x2 < W;) {
      if (!page_px[y][x2]) {
        x2++;
        continue;
      }
      int start = x2;
      while (x2 < W && page_px[y][x2]) x2++;
      heads[2 * n] = (uint8_t)start;
      heads[2 * n + 1] = (uint8_t)(x2 - start - 1);
      n++;
    }
    *count = (uint8_t)n;
    out = heads + 2 * n;
    for (int k = 0; k < n; k++) {
      int start = heads[2 * k], len = heads[2 * k + 1] + 1;
      memcpy(out, &page_px[y][start], (size_t)len);
      out += len;
    }
  }
}

static void make_sprite(void) {
  for (int r = 0; r < SCENE_SPRITE; r++) {
    for (int c = 0; c < SCENE_SPRITE; c++) {
      sprite_px[r][c] = (next() % 2) ? 0 : (uint8_t)(1 + next() % 15);
    }
  }
  for (int r = 0; r < SCENE_SPRITE; r++) {
    for (int c = 0; c < SCENE_SPRITE; c += 2) {
      sprite4[r * 8 + c / 2] = (uint8_t)(sprite_px[r][c] << 4 | sprite_px[r][c + 1]);
    }
  }
  uint8_t *out = sprite_rle;
  for (int r = 0; r < SCENE_SPRITE; r++) {
    uint8_t *count = out++;
    int n = 0;
    for (int c = 0; c < SCENE_SPRITE;) {
      if (!sprite_px[r][c]) {
        c++;
        continue;
      }
      int start = c;
      while (c < SCENE_SPRITE && sprite_px[r][c]) c++;
      *out++ = (uint8_t)(start << 4 | (c - start - 1));
      memcpy(out, &sprite_px[r][start], (size_t)(c - start));
      out += c - start;
      n++;
    }
    *count = (uint8_t)n;
  }
}

static void fill_noise(void) {
  for (int i = 0; i < W * W; i++) fb[i] = ref[i] = (uint8_t)(0x80 | (i * 7));
}

static void ref_page(int scroll, int y0, int y1, int key) {
  for (int y = y0 < 0 ? 0 : y0; y < (y1 > W ? W : y1); y++) {
    for (int x = 0; x < W; x++) {
      uint8_t v = page_px[(scroll + y) % LINES][x];
      if (!key || v) ref[y * W + x] = v;
    }
  }
}

static void ref_sprite(int sx, int sy, int y0, int y1, const uint8_t *lut) {
  for (int r = 0; r < SCENE_SPRITE; r++) {
    int y = sy + r;
    if (y < 0 || y >= W || y < y0 || y >= y1) continue;
    for (int c = 0; c < SCENE_SPRITE; c++) {
      int x = sx + c;
      uint8_t v = sprite_px[r][c];
      if (x < 0 || x >= W || !v) continue;
      ref[y * W + x] = lut ? lut[v] : v;
    }
  }
}

static int same(void) { return memcmp(fb, ref, sizeof fb) == 0; }

int main(void) {
  scene_draw_init();
  make_page();

  /* Pages: every scroll, whole and in bands, opaque, keyed, as runs. */
  int bands[][2] = {{0, 256}, {0, 128}, {128, 256}, {-5, 3}, {250, 300}, {17, 18}};
  for (int scroll = 0; scroll < LINES; scroll += 3) {
    for (size_t b = 0; b < sizeof bands / sizeof bands[0]; b++) {
      int y0 = bands[b][0], y1 = bands[b][1];
      fill_noise();
      scene_page4(fb, page4, LINES, scroll, y0, y1);
      ref_page(scroll, y0, y1, 0);
      CHECK(same());
      fill_noise();
      scene_page4_key(fb, page4, LINES, scroll, y0, y1);
      ref_page(scroll, y0, y1, 1);
      CHECK(same());
      fill_noise();
      scene_page_runs(fb, runs, index_, LINES, scroll, y0, y1);
      ref_page(scroll, y0, y1, 1);
      CHECK(same());
    }
  }

  /* Sprites: random patterns at random positions across every edge, in
   * random bands, through a table and as runs. */
  uint8_t lut[16];
  for (int i = 0; i < 16; i++) lut[i] = (uint8_t)(i ? 1 + (i + 4) % 15 : 0);
  for (int t = 0; t < 4000; t++) {
    make_sprite();
    int x = (int)(next() % 300) - 22, y = (int)(next() % 300) - 22;
    int y0 = (int)(next() % 280) - 12, y1 = y0 + (int)(next() % 260);
    fill_noise();
    scene_sprite4(fb, sprite4, lut, x, y, y0, y1);
    ref_sprite(x, y, y0 < 0 ? 0 : y0, y1 > W ? W : y1, lut);
    CHECK(same());
    fill_noise();
    scene_sprite_rle(fb, sprite_rle, x, y, y0, y1);
    ref_sprite(x, y, y0 < 0 ? 0 : y0, y1 > W ? W : y1, NULL);
    CHECK(same());
  }

  /* Two bands drawn apart make the frame one band draws. */
  make_sprite();
  fill_noise();
  for (int k = 0; k < 64; k++) {
    int x = (int)(next() % 300) - 22, y = (int)(next() % 300) - 22;
    scene_sprite_rle(fb, sprite_rle, x, y, 0, 128);
    scene_sprite_rle(fb, sprite_rle, x, y, 128, 256);
    ref_sprite(x, y, 0, 256, NULL);
  }
  CHECK(same());

  TEST_END();
}
