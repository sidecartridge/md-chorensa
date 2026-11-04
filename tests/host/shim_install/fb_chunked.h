/* Host stand-in for fb_chunked.h (test_install): the frame drawn in bands. */
#pragma once
#include <stdint.h>
#define FB_CHUNKED_W 256
#define FB_CHUNKED_H 256
typedef void (*fb_band_draw_t)(uint8_t *band, const void *arg, int y0, int y1);
void fb_draw_banded(fb_band_draw_t draw, const void *arg);
