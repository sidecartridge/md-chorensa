/**
 * File: bench.c
 * Description: The measurement bench. See bench.h.
 */

#include "bench.h"

#include <stdio.h>

#include "cart_shared.h"
#include "debug.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "palette.h"
#include "pico/time.h"
#include "st_session.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */

#define COL_BG 0
#define COL_TEXT 1
#define COL_TITLE 2
#define COL_LABEL 3

static const uint16_t s_palette[PALETTE_ENTRIES] = {
    PALETTE_RGB(0, 0, 1), PALETTE_RGB(7, 7, 7), PALETTE_RGB(7, 4, 0),
    PALETTE_RGB(4, 4, 5),
};

/* Frames published since the ST's boot, and the rate over the last second
 * (also readable over SWD by symbol). */
uint32_t benchFrames;
uint32_t benchFramesPerSecond;
static uint32_t s_second_start_us;
static uint32_t s_second_frames;

static const char *machine_name(uint8_t machine) {
  switch (machine) {
    case ST_MACHINE_ST:
      return "ST";
    case ST_MACHINE_STE:
      return "STE";
    case ST_MACHINE_MEGASTE:
      return "MEGA STE";
    case ST_MACHINE_TT:
      return "TT";
    case ST_MACHINE_FALCON:
      return "FALCON";
    default:
      return "?";
  }
}

static void print_at(int col, int row, unsigned color, const char *text) {
  font_set_color(color);
  font_move((unsigned)col * 8u, (unsigned)row * 8u);
  font_print(text);
}

void bench_init(void) {
  palette_set(s_palette);
  bench_restart();
  DPRINTF("bench: ready\n");
}

void bench_restart(void) {
  benchFrames = 0;
  benchFramesPerSecond = 0;
  s_second_frames = 0;
  s_second_start_us = time_us_32();
  DPRINTF("bench: new ST session\n");
}

void bench_handle_key(const ikbd_key_event_t *key) { (void)key; }

void bench_frame(void) {
  char line[48];

  fb_chunked_clear(COL_BG);
  font_set_font(&font8x8);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);

  print_at(1, 1, COL_TITLE, "CHO REN SHA  BENCH");

  print_at(1, 4, COL_LABEL, "ST");
  if (st_session_hellos() == 0) {
    print_at(10, 4, COL_TEXT, "NO HELLO YET");
  } else {
    uint16_t tos = st_session_tos_version();
    snprintf(line, sizeof line, "%s  TOS %X.%02X  %s",
             machine_name(st_session_machine()), (unsigned)(tos >> 8),
             (unsigned)(tos & 0xFFu),
             (st_session_features() & CART_ST_FEATURE_BLITTER) ? "BLITTER"
                                                                 : "NO BLITTER");
    print_at(10, 4, COL_TEXT, line);
  }

  print_at(1, 6, COL_LABEL, "FRAMES");
  snprintf(line, sizeof line, "%lu  %lu/S", (unsigned long)benchFrames,
           (unsigned long)benchFramesPerSecond);
  print_at(10, 6, COL_TEXT, line);

  print_at(1, 8, COL_LABEL, "C2P");
  snprintf(line, sizeof line, "%lu US", (unsigned long)fb_last_convert_us());
  print_at(10, 8, COL_TEXT, line);

  print_at(1, 23, COL_LABEL, "ESC: BACK TO GEM");

  fb_publish();

  benchFrames++;
  s_second_frames++;
  uint32_t now = time_us_32();
  if (now - s_second_start_us >= 1000000u) {
    benchFramesPerSecond = s_second_frames;
    s_second_frames = 0;
    s_second_start_us = now;
  }
}

uint32_t bench_devhook(uint16_t commandId, const uint16_t *payload,
                       uint16_t payloadSize) {
  (void)payload;
  (void)payloadSize;
  switch (commandId) {
    case DEVHOOKS_APP_BENCH_PING:
      return benchFrames + 1u;
    default:
      return 0;
  }
}
