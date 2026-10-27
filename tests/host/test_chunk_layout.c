// units: rp/src/fb_chunked.c emu/fb_workers.c
/* The framebuffer's layouts, end to end: what the app draws in the chunky
 * buffer is what the ST shows.
 *
 * fb_transpose() turns the chunky buffer into the ST's planes on both cores
 * (core 1 is a thread here) and fb_planar_publish() copies them to the
 * cartridge framebuffer in the copier's layout (cart_shared.h, "Framebuffer
 * layout for the m68k's copy"): for the MOVEM copy the lines from the last
 * to the first, each as its bytes 80..127, 32..79 and 0..31, because
 * FBDRV_INLINE stores them with predecrementing MOVEMs into a screen page of
 * 160-byte lines; for the blitter the planes as they are, which it copies a
 * line at a time, its Y increment skipping the bytes right of the image.
 * This test runs the RP's side, models both copies from the constants of
 * cart_shared.h (the blitter in one run and in pieces), decodes the ST's
 * planes and compares every pixel with what was drawn; the 32 bytes right of
 * the image in each line stay untouched.
 *
 * The c2p worker is Thumb assembly on the RP (fb_chunked_asm.S); here it is
 * the C it stands for (emu/fb_workers.c). A frame drawn in bands
 * (fb_draw_banded), converted band by band in the buffer's first part, shows
 * the same. */
#include <stdbool.h>
#include <string.h>

#include "cart_shared.h"
#include "fb_chunked.h"
#include "test.h"

#define W FB_CHUNKED_W
#define H FB_CHUNKED_H

/* The cartridge framebuffer, as 16-bit words (the ST reads the RP's words
 * with their bytes swapped, which leaves their values as they are), and a
 * screen page of CART_FB_LINES lines of the ST's 160 bytes. */
#define ST_LINE_BYTES 160
static uint16_t cart[CART_FRAMEBUFFER_SIZE / 2];
static uint16_t screen[CART_FB_LINES * ST_LINE_BYTES / 2];
/* The frame as drawn: fb_transpose() converts the chunky buffer in place,
 * so each conversion starts from this copy, drawn again (as an app draws
 * every frame whole). */
static uint8_t drawn[FB_CHUNKED_SIZE];

static void draw_again(void) { memcpy(fb_chunked_buffer, drawn, sizeof drawn); }

/* FBDRV_INLINE: from the page's last line to its first, a predecrementing
 * MOVEM of 48 bytes to the line's bytes 80..127, one to 32..79, one of 32
 * bytes to 0..31, each read forward from the cartridge. */
static void m68k_copy_lines(uint32_t lines) {
  /* The top `lines` only: the framebuffer holds the lines from the last to
   * the first, so the copy starts past the bottom ones (fbdrv_short). */
  const uint16_t *src = cart + (CART_FB_LINES - lines) * CART_FB_LINE_BYTES / 2;
  for (int y = (int)lines - 1; y >= 0; y--) {
    uint16_t *end = &screen[(y * ST_LINE_BYTES + CART_FB_LINE_BYTES) / 2];
    end -= CART_FB_CHUNK_BYTES / 2;
    memcpy(end, src, CART_FB_CHUNK_BYTES);
    src += CART_FB_CHUNK_BYTES / 2;
    end -= CART_FB_CHUNK_BYTES / 2;
    memcpy(end, src, CART_FB_CHUNK_BYTES);
    src += CART_FB_CHUNK_BYTES / 2;
    end -= CART_FB_LINE_TAIL / 2;
    memcpy(end, src, CART_FB_LINE_TAIL);
    src += CART_FB_LINE_TAIL / 2;
  }
}

static void m68k_copy(void) { m68k_copy_lines(CART_FB_LINES); }

static uint8_t st_pixel(int x, int y) {
  const uint16_t *group = &screen[y * (ST_LINE_BYTES / 2) + (x / 16) * 4];
  int bit = 15 - (x % 16);
  return (uint8_t)(((group[0] >> bit) & 1) | (((group[1] >> bit) & 1) << 1) |
                   (((group[2] >> bit) & 1) << 2) |
                   (((group[3] >> bit) & 1) << 3));
}

static int shows_what_was_drawn_to(int lines) {
  for (int y = 0; y < lines; y++) {
    for (int x = 0; x < W; x++) {
      if (st_pixel(x, y) != (drawn[y * W + x] & 0x0Fu)) {
        fprintf(stderr, "pixel (%d, %d): ST shows %u, drawn %u\n", x, y,
                st_pixel(x, y), drawn[y * W + x] & 0x0Fu);
        return 0;
      }
    }
  }
  return 1;
}

static int shows_what_was_drawn(void) { return shows_what_was_drawn_to(H); }

/* The screen's lines from `from` down hold `fill`: a short copy left them. */
static int lines_untouched(int from, uint8_t fill) {
  const uint8_t *bytes = (const uint8_t *)screen;
  for (int y = from; y < CART_FB_LINES; y++) {
    for (int b = 0; b < CART_FB_LINE_BYTES; b++) {
      if (bytes[y * ST_LINE_BYTES + b] != fill) {
        fprintf(stderr, "line %d byte %d written by a copy of %d lines\n", y, b, from);
        return 0;
      }
    }
  }
  return 1;
}

/* The blitter as userfw drives it (HOP source, OP source, all endmasks
 * set): word by word, adding the X increment after each word of a line but
 * the last and the Y increment after the last; its addresses carry on from
 * one piece to the next. Addresses are byte offsets into the cartridge
 * framebuffer and the screen page. */
typedef struct {
  int32_t src, dst;
  int32_t src_x_inc, src_y_inc, dst_x_inc, dst_y_inc;
  uint32_t x_count;
} blitter_t;

static void blitter_run(blitter_t *b, uint32_t y_count) {
  for (uint32_t y = 0; y < y_count; y++) {
    for (uint32_t x = 0; x < b->x_count; x++) {
      screen[b->dst / 2] = cart[b->src / 2];
      bool last = x + 1u == b->x_count;
      b->src += last ? b->src_y_inc : b->src_x_inc;
      b->dst += last ? b->dst_y_inc : b->dst_x_inc;
    }
  }
}

/* userfw's blitter copy: a line at a time from the image's first line,
 * `piece` lines per start. */
static void blitter_copy_lines(uint32_t piece, uint32_t lines) {
  blitter_t b = {
      .src = 0,
      .dst = 0,
      .src_x_inc = 2,
      .src_y_inc = 2,
      .dst_x_inc = 2,
      .dst_y_inc = CART_FB_BLITTER_DST_Y_INC,
      .x_count = CART_FB_LINE_BYTES / 2,
  };
  for (uint32_t left = lines; left > 0;) {
    uint32_t n = left < piece ? left : piece;
    blitter_run(&b, n);
    left -= n;
  }
}

static void blitter_copy(uint32_t piece) { blitter_copy_lines(piece, CART_FB_LINES); }

static int screen_right_of_image_untouched(uint8_t fill) {
  const uint8_t *bytes = (const uint8_t *)screen;
  for (int y = 0; y < CART_FB_LINES; y++) {
    for (int b = CART_FB_LINE_BYTES; b < ST_LINE_BYTES; b++) {
      if (bytes[y * ST_LINE_BYTES + b] != fill) {
        fprintf(stderr, "line %d byte %d written\n", y, b);
        return 0;
      }
    }
  }
  return 1;
}

/* Every copy of the drawn frame shows it and leaves the bytes right of the
 * image as they were: the MOVEM copy of its layout, and the blitter's of the
 * natural one, in one run and in pieces of the ST's default, of one line, of
 * an odd size and of the largest the RP can ask for. The screen starts as
 * garbage each time: every byte of the image must be copied. */
static int every_copy_shows_what_was_drawn(void) {
  static const uint32_t pieces[] = {CART_FB_LINES, CART_BLIT_PIECE_DEFAULT, 1,
                                    7, 255};
  draw_again();
  fb_transpose();
  fb_planar_publish(cart, false);
  memset(screen, 0x5A, sizeof(screen));
  m68k_copy();
  if (!shows_what_was_drawn() || !screen_right_of_image_untouched(0x5A)) {
    fprintf(stderr, "the MOVEM copy\n");
    return 0;
  }
  fb_planar_publish(cart, true);
  for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
    memset(screen, 0xA5, sizeof(screen));
    blitter_copy(pieces[i]);
    if (!shows_what_was_drawn() || !screen_right_of_image_untouched(0xA5)) {
      fprintf(stderr, "the blitter, pieces of %u lines\n", (unsigned)pieces[i]);
      return 0;
    }
  }
  return 1;
}

/* A copy of the frame's top CART_FB_LINES_SHORT lines, by either copier,
 * shows them and leaves the screen's lines below as they were. */
static int short_copy_shows_the_top_lines(void) {
  draw_again();
  fb_transpose();
  fb_planar_publish(cart, false);
  memset(screen, 0x5A, sizeof(screen));
  m68k_copy_lines(CART_FB_LINES_SHORT);
  if (!shows_what_was_drawn_to(CART_FB_LINES_SHORT) ||
      !lines_untouched(CART_FB_LINES_SHORT, 0x5A)) {
    fprintf(stderr, "the short MOVEM copy\n");
    return 0;
  }
  fb_planar_publish(cart, true);
  memset(screen, 0xA5, sizeof(screen));
  blitter_copy_lines(CART_BLIT_PIECE_DEFAULT, CART_FB_LINES_SHORT);
  if (!shows_what_was_drawn_to(CART_FB_LINES_SHORT) ||
      !lines_untouched(CART_FB_LINES_SHORT, 0xA5)) {
    fprintf(stderr, "the short blitter copy\n");
    return 0;
  }
  return 1;
}

/* fb_draw_banded: each band asked for once, a whole band of lines in its
 * half, drawn from `drawn`; the buffer past FB_BANDED_BYTES left alone. */
static uint32_t band_calls;
static int band_ok = 1;

static void draw_band(uint8_t *band, const void *arg, int y0, int y1) {
  (void)arg;
  __atomic_fetch_add(&band_calls, 1u, __ATOMIC_RELAXED);
  if (y1 - y0 != FB_BAND_LINES || y0 % FB_BAND_LINES != 0 || band < fb_chunked_buffer ||
      band + (y1 - y0) * W > fb_chunked_buffer + FB_BANDED_BYTES) {
    band_ok = 0;
    return;
  }
  memcpy(band, drawn + y0 * W, (size_t)(y1 - y0) * W);
}

static int banded_copy_shows_what_was_drawn(void) {
  memset(fb_chunked_buffer, 0x77, sizeof fb_chunked_buffer);
  band_calls = 0;
  fb_draw_banded(draw_band, NULL);
  if (!band_ok || band_calls != H / FB_BAND_LINES) {
    fprintf(stderr, "bands: %u calls, %s\n", (unsigned)band_calls, band_ok ? "in place" : "out of place");
    return 0;
  }
  for (size_t i = FB_BANDED_BYTES; i < sizeof fb_chunked_buffer; i++) {
    if (fb_chunked_buffer[i] != 0x77) {
      fprintf(stderr, "byte %u past the banded part written\n", (unsigned)i);
      return 0;
    }
  }
  fb_planar_publish(cart, false);
  memset(screen, 0x5A, sizeof(screen));
  m68k_copy();
  if (!shows_what_was_drawn() || !screen_right_of_image_untouched(0x5A)) {
    fprintf(stderr, "the banded frame, the MOVEM copy\n");
    return 0;
  }
  fb_planar_publish(cart, true);
  memset(screen, 0xA5, sizeof(screen));
  blitter_copy(CART_BLIT_PIECE_DEFAULT);
  if (!shows_what_was_drawn() || !screen_right_of_image_untouched(0xA5)) {
    fprintf(stderr, "the banded frame, the blitter\n");
    return 0;
  }
  return 1;
}

int main(void) {
  /* The copy covers the whole framebuffer, as userfw's FBDRV_INLINE does
   * (tests/host/test_layout.py checks the two sides agree). */
  CHECK_EQ(CART_FB_LINES * CART_FB_LINE_BYTES, CART_FRAMEBUFFER_SIZE);
  CHECK_EQ(CART_FB_CHUNKS_A_LINE * CART_FB_CHUNK_BYTES + CART_FB_LINE_TAIL,
           CART_FB_LINE_BYTES);
  CHECK_EQ(CART_FB_BLITTER_DST_Y_INC, ST_LINE_BYTES - CART_FB_LINE_BYTES + 2);

  fb_chunked_init(); /* core 1 */

  /* A pattern with every colour in every position, a gradient, a single
   * pixel at each corner, and the last line. */
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      drawn[y * W + x] = (uint8_t)((x * 7 + y * 3) & 0x0F);
    }
  }
  CHECK(every_copy_shows_what_was_drawn());
  CHECK(short_copy_shows_the_top_lines());
  /* Drawn in bands, then a whole frame again: each publishes its own. */
  CHECK(banded_copy_shows_what_was_drawn());
  CHECK(every_copy_shows_what_was_drawn());
  /* In place: after the conversion the chunky buffer holds planes, not the
   * pixels drawn. */
  CHECK(memcmp(fb_chunked_buffer, drawn, sizeof drawn) != 0);

  memset(drawn, 0, sizeof drawn);
  drawn[0] = 1;
  drawn[W - 1] = 2;
  drawn[(H - 1) * W] = 3;
  drawn[H * W - 1] = 4;
  for (int x = 0; x < W; x++) drawn[(H - 1) * W + x] = (uint8_t)(x & 15);
  CHECK(every_copy_shows_what_was_drawn());
  CHECK(banded_copy_shows_what_was_drawn());

  /* The high nibble of a chunky byte is not a colour: the ST sees only the
   * palette index in the low nibble. */
  for (int i = 0; i < W * H; i++) drawn[i] = (uint8_t)(0xA0 | (i % 11));
  CHECK(every_copy_shows_what_was_drawn());

  TEST_END();
}
