/**
 * File: xl_render.h
 * Description: The translated game's picture in the ST's 16 colours: the
 *              X68000's screen as the Amiga port draws it (the two graphics
 *              pages, page 0 in front, the backdrop where both are clear, then
 *              the text plane and the sprites in the order R1 gives), from the
 *              game's memory and the stage's art (tools/xlat/xl_store.h).
 *
 * Plain C over tools/xlat/xl.h's memory accessors, so that the host's
 * runtime (tools/xlat/host/xl_host.c) draws the same picture from the same
 * source and checks it against the host's own renderer. Draws the lines
 * [y0, y1) of a 256 x 256 chunky picture (a byte a pixel, the ST colour
 * number): the RP draws its frame in bands on two cores (fb_draw_banded).
 */

#ifndef XL_RENDER_H
#define XL_RENDER_H

#include <stdint.h>

#include "xl_store.h"

#define XL_RENDER_W 256
#define XL_RENDER_H 256
#define XL_RENDER_SPRITES 512

/* What a frame shows, taken when the game hands its sprite list over
 * (XSP_OUT) and at its frame wait (XSP_VSYNC). */
typedef struct {
  const xl_store_art_t *art;
  uint32_t sprite_count;
  uint16_t sprites[XL_RENDER_SPRITES][4]; /* x, y, pattern, attributes */
  uint16_t order[XL_RENDER_SPRITES];      /* drawing order (xl_render_prepare) */
  uint16_t page0_y, page1_y;              /* the CRTC's vertical scrolls */
  uint16_t text_x, text_y;
  int text_in_front;                      /* R1: text over the sprites */
} xl_render_frame_t;

/* The sprite list, from the game's table at `table` (`count` entries). */
void xl_render_take_sprites(xl_render_frame_t *f, uint32_t table, uint32_t count);
/* The scrolls and R1 from the game's registers, and the drawing order. */
void xl_render_prepare(xl_render_frame_t *f);
/* Draw the lines [y0, y1) of the frame at `band`: line y0's first pixel,
 * XL_RENDER_W bytes a line. */
void xl_render_band(uint8_t *band, const xl_render_frame_t *f, int y0, int y1);

/* The 16 ST colours (0RRR0GGG0BBB) for the program's current palettes: per
 * colour, its entries' current colours weighted (palette_fit.py's
 * arithmetic). Returns 1 when they changed since the last call. */
int xl_render_palette(const xl_store_art_t *art, uint16_t out[16]);

#endif /* XL_RENDER_H */
