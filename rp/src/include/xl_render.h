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

#ifndef XL_RENDER_HEADER
#define XL_RENDER_HEADER

#include <stdint.h>

#include "xl_store.h"

#define XL_RENDER_W 256
#define XL_RENDER_H 256
#define XL_RENDER_SPRITES 512
/* The pages of the game's sprite list (8 bytes a sprite) at most. */
#define XL_RENDER_SPRITE_PAGES (XL_RENDER_SPRITES * 8 / 256 + 1)

/* What a frame shows, taken when the game hands its sprite list over
 * (XSP_OUT) and at its frame wait (XSP_VSYNC). Each sprite: x, y, pattern,
 * attributes. The RP reads them where the game keeps them (the game does not
 * write its list again before the frame is drawn), the host from a copy. */
typedef struct {
  const xl_store_art_t *art;
  uint32_t sprite_count;
#ifdef XL_PAGED
  const uint8_t *sprite_pages[XL_RENDER_SPRITE_PAGES]; /* the list's pages */
  uint32_t sprite_offset;                              /* its start in the first */
#else
  uint16_t sprites[XL_RENDER_SPRITES][4];
#endif
  uint16_t order[XL_RENDER_SPRITES];      /* drawing order (xl_render_prepare) */
  uint16_t page0_y, page1_y;              /* the CRTC's vertical scrolls */
  uint16_t text_x, text_y;
  int text_in_front;                      /* R1: text over the sprites */
  uint32_t skip;                          /* XL_RENDER_SKIP_*: parts left out (0: none) */
} xl_render_frame_t;

/* Parts of the picture a frame can leave out, to time the others (the
 * bench's held frame). */
#define XL_RENDER_SKIP_PAGES 1u
#define XL_RENDER_SKIP_TEXT 2u
#define XL_RENDER_SKIP_SPRITES 4u

/* The sprite list, from the game's table at `table` (`count` entries). */
void xl_render_take_sprites(xl_render_frame_t *f, uint32_t table, uint32_t count);
/* The scrolls and R1 from the game's registers, and the drawing order. */
void xl_render_prepare(xl_render_frame_t *f);
/* Draw the lines [y0, y1) of the frame at `band`: line y0's first pixel,
 * XL_RENDER_W bytes a line. */
void xl_render_band(uint8_t *band, const xl_render_frame_t *f, int y0, int y1);
/* The same, with page 1's lines for the band already in RAM: y1 - y0 lines
 * of XL_RENDER_W / 2 bytes, word-aligned, as page 1 holds them. They may lie
 * in the band's own second half (from line (y1 - y0) / 2 on), which the
 * drawing overwrites only after reading them; NULL: from the art. */
void xl_render_band_page1(uint8_t *band, const xl_render_frame_t *f, int y0, int y1,
                          const uint8_t *page1_lines);

/* The 16 ST colours (0RRR0GGG0BBB) for the program's current palettes: per
 * colour, its entries' current colours weighted (palette_fit.py's
 * arithmetic). Returns 1 when they changed since the last call. */
int xl_render_palette(const xl_store_art_t *art, uint16_t out[16]);
/* The next xl_render_palette works the colours out whatever the palette's
 * words: a run starting over (the ST shows another palette by then), or
 * another scene's art (which a scene change puts where the last one was). */
void xl_render_palette_forget(void);

#endif /* XL_RENDER_HEADER */
