/**
 * File: scene_draw.h
 * Description: The bench scene's drawing primitives, each in the formats the
 *              bench compares (bench_scene.c): background pages 256 pixels
 *              wide, scrolled vertically and wrapped, and 16 x 16 sprites.
 *
 * Every primitive draws into a 256-pixel-wide chunky buffer (one byte a
 * pixel, the ST colour number), clipped to the buffer's width and to the
 * lines [y0, y1): two cores draw two bands of one frame. Colour 0 is
 * transparent wherever a primitive has transparency. Formats:
 *
 *   4-bit page   two pixels a byte, the left one in the high nibble, 128
 *                bytes a line;
 *   run page     per line a record: the number of runs n, n pairs (x,
 *                length - 1), then the runs' pixels, a byte each; an index of
 *                each line's offset;
 *   8-bit sprite 256 bytes, row after row (fb_blit_key's format);
 *   4-bit sprite 128 bytes, as the 4-bit page, drawn through a 16-entry
 *                table (the sprite's palette);
 *   run sprite   per row: the number of runs, then each run as one byte
 *                (x << 4 | length - 1) and its pixels.
 */

#ifndef SCENE_DRAW_H
#define SCENE_DRAW_H

#include <stdint.h>

#define SCENE_W 256
#define SCENE_SPRITE 16

/* The 4-bit to 8-bit table the 4-bit pages use: call once. */
void scene_draw_init(void);

/* A 4-bit page of `lines` lines, the page's line `scroll` at the buffer's
 * line 0, opaque. */
void scene_page4(uint8_t *fb, const uint8_t *page, int lines, int scroll, int y0,
                 int y1);
/* The same with colour 0 transparent (a colour-key blit). */
void scene_page4_key(uint8_t *fb, const uint8_t *page, int lines, int scroll,
                     int y0, int y1);
/* A run page: only its opaque runs are copied. */
void scene_page_runs(uint8_t *fb, const uint8_t *runs, const uint32_t *index,
                     int lines, int scroll, int y0, int y1);

/* A 4-bit sprite at (x, y), through `lut`. */
void scene_sprite4(uint8_t *fb, const uint8_t *pattern, const uint8_t *lut, int x,
                   int y, int y0, int y1);
/* A run sprite at (x, y). */
void scene_sprite_rle(uint8_t *fb, const uint8_t *rle, int x, int y, int y0,
                      int y1);

#endif /* SCENE_DRAW_H */
