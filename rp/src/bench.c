/**
 * File: bench.c
 * Description: The measurement bench. See bench.h.
 */

#include "bench.h"

#include <math.h>
#include <stdio.h>

#include "audio.h"
#include "bench_logic.h"
#include "bench_scene.h"
#include "cart_shared.h"
#include "debug.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "palette.h"
#include "pico/time.h"
#include "st_session.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */
extern uint32_t fbBorderFrames[4];   /* fb.c */
extern volatile uint32_t ikbdBytes;  /* ikbd.c */

#define W FB_CHUNKED_W
#define H FB_CHUNKED_H

/* Sixteen distinct colours, black first: the test card shows every entry. */
#define COL_BG 0
#define COL_TEXT 15
#define COL_TITLE 14
#define COL_LABEL 7

static const uint16_t s_palette[PALETTE_ENTRIES] = {
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(7, 0, 0), PALETTE_RGB(0, 7, 0),
    PALETTE_RGB(0, 0, 7), PALETTE_RGB(7, 7, 0), PALETTE_RGB(0, 7, 7),
    PALETTE_RGB(7, 0, 7), PALETTE_RGB(3, 3, 3), PALETTE_RGB(7, 3, 0),
    PALETTE_RGB(3, 7, 0), PALETTE_RGB(0, 3, 7), PALETTE_RGB(7, 0, 3),
    PALETTE_RGB(3, 0, 7), PALETTE_RGB(5, 5, 5), PALETTE_RGB(7, 5, 0),
    PALETTE_RGB(7, 7, 7),
};

/* Frames published since the ST's boot, and the rate over the last second
 * (also readable over SWD by symbol). */
uint32_t benchFrames;
uint32_t benchFramesPerSecond;
/* The still mode (bench_still, or benchStill written over SWD in any
 * build): the bench stops drawing and publishes what the chunky buffer
 * holds, with this palette, every frame. A host tool (tools/dev/swd.py
 * still) writes all three over SWD: a picture to look at on the ST's own
 * screen. */
uint16_t benchStillPalette[PALETTE_ENTRIES];
volatile uint32_t benchStill;
static bool s_still; /* what the last frame showed */
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

static void print_at(int x, int y, unsigned color, const char *text) {
  font_set_color(color);
  font_move((unsigned)x, (unsigned)y);
  font_print(text);
}

/* F5: the ST plays no sound at all from its next boot, or plays again
 * (audio_silence_st()): what its frame costs without the samples. F6: a
 * test tone through the PCM path, on and off (audio_set_pcm_callback()): a
 * sine, whose clicks and jitter are easy to hear or to see in a recording. */
#define SC_F5 0x3Fu
#define SC_F6 0x40u
#define TONE_DEFAULT_HZ 440u
/* F7: the copy mode, round: auto (the blitter on the DMA sound path), the
 * CPU, the blitter (fb_set_copy_mode()). F8: the lines a blitter piece,
 * round: the ST's default, 32, 4, 8. */
#define SC_F7 0x41u
#define SC_F8 0x42u
/* F9: the lines the ST copies, all 256 or the top 240 (fb_set_copy_lines()):
 * the ST family's budget with sampled sound. */
#define SC_F9 0x43u
/* F10: the ST keeps to the YM from its next boot, or back to the DMA chip
 * where there is one (audio_prefer_ym()): the ST family's sound path on an
 * STE. */
#define SC_F10 0x44u
/* HELP: the scene's sweep (bench_scene.h), its table at the end; UNDO: back
 * to the test card. */
#define SC_HELP 0x62u
#define SC_UNDO 0x61u
/* INSERT: the logic run (bench_logic.h), in a build with a translation. */
#define SC_INSERT 0x52u
static bool s_bench_prefer_ym; /* F10's state (audio.c keeps its own) */
/* The same settings over SWD in any build (tools/dev/swd.py bench-set),
 * taken at the next bench frame and cleared: the copy mode + 1 (and its
 * lines a blitter piece << 8), the lines the ST copies, the sound path
 * from the ST's next boot (1 its own, 2 the YM). 0 leaves one as it is. */
volatile uint32_t benchSetCopyMode;
volatile uint32_t benchSetCopyLines;
volatile uint32_t benchSetSoundPath;
static const uint8_t k_pieces[] = {0, 32, 4, 8};
static uint8_t s_piece_choice;

extern uint32_t fbLayoutNatural; /* fb.c */
static uint8_t s_copy_mode;
static uint8_t s_copy_piece;
static const char *const k_copy_modes[] = {"AUTO", "CPU", "BLITTER"};

static void set_copy_lines(uint16_t lines) {
  fb_set_copy_lines(lines);
  DPRINTF("bench: the ST copies %u lines\n", (unsigned)fb_copy_lines());
}

static void set_copy_mode(uint8_t mode, uint8_t piece) {
  s_copy_mode = mode;
  s_copy_piece = piece;
  fb_set_copy_mode(mode, piece);
  DPRINTF("bench: copy mode %s, %u lines a piece\n", k_copy_modes[mode],
          (unsigned)piece);
}

static bool s_st_silent;
static int8_t s_sine[256];
static uint32_t s_tone_phase;
static uint32_t s_tone_step; /* 0: no tone */

static void tone_cb(int8_t *buf, uint32_t samples) {
  for (uint32_t i = 0; i < samples; i++) {
    buf[i] = s_sine[s_tone_phase >> 24];
    s_tone_phase += s_tone_step;
  }
}

static void set_tone(uint32_t hz) {
  s_tone_step = hz ? (uint32_t)(((uint64_t)hz << 32) / AUDIO_DMA_RATE_HZ) : 0u;
  audio_set_pcm_callback(hz ? tone_cb : NULL, AUDIO_DMA_RATE_HZ);
  DPRINTF("bench: tone %lu Hz\n", (unsigned long)hz);
}

static void set_silent(bool silent) {
  s_st_silent = silent;
  audio_silence_st(silent);
}

void bench_init(void) {
  bench_scene_init();
  for (int i = 0; i < 256; i++) {
    s_sine[i] = (int8_t)lround(100.0 * sin(2.0 * M_PI * i / 256.0));
  }
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

/* F1..F4: the IKBD's input mode (ikbd_set_input_mode()): keys only, the
 * mouse, the mouse and stick 1, both sticks; the mouse moved flat out loads
 * the ST with IKBD interrupts. */
#define SC_F1 0x3Bu
#define SC_F4 0x3Eu
void bench_handle_key(const ikbd_key_event_t *key) {
  if (!key->is_press) return;
  if (key->scancode >= SC_F1 && key->scancode <= SC_F4) {
    ikbd_set_input_mode((ikbd_input_mode_t)(key->scancode - SC_F1));
  } else if (key->scancode == SC_F5) {
    set_silent(!s_st_silent);
  } else if (key->scancode == SC_F6) {
    set_tone(s_tone_step ? 0u : TONE_DEFAULT_HZ);
  } else if (key->scancode == SC_F7) {
    set_copy_mode((uint8_t)((s_copy_mode + 1u) % 3u), s_copy_piece);
  } else if (key->scancode == SC_F8) {
    s_piece_choice = (uint8_t)((s_piece_choice + 1u) % 4u);
    set_copy_mode(s_copy_mode, k_pieces[s_piece_choice]);
  } else if (key->scancode == SC_F9) {
    set_copy_lines(fb_copy_lines() == CART_FB_LINES ? CART_FB_LINES_SHORT
                                                    : CART_FB_LINES);
  } else if (key->scancode == SC_INSERT) {
    bench_logic_run();
  } else if (key->scancode == SC_HELP) {
    bench_scene_start(BENCH_SCENE_FRAMES_DEFAULT);
  } else if (key->scancode == SC_UNDO) {
    if (bench_scene_active()) {
      bench_scene_stop();
      palette_set(s_palette);
    }
  } else if (key->scancode == SC_F10) {
    s_bench_prefer_ym = !s_bench_prefer_ym;
    audio_prefer_ym(s_bench_prefer_ym);
  }
}

/* The test card: a frame on the first and last line and column, a numbered
 * tick every 16 lines, the sixteen colours as 16-pixel columns, and a bar
 * moving down a line a frame, so that a missing line or column, a palette
 * entry or a dropped frame shows at once. */
static void draw_test_card(void) {
  char label[8];

  fb_chunked_clear(COL_BG);
  fb_fill_rect(0, 0, W, 1, COL_TEXT);
  fb_fill_rect(0, H - 1, W, 1, COL_TEXT);
  fb_fill_rect(0, 0, 1, H, COL_TEXT);
  fb_fill_rect(W - 1, 0, 1, H, COL_TEXT);
  for (int c = 0; c < 16; c++) {
    fb_fill_rect(c * 16, 168, 16, 48, (uint8_t)c);
  }
  for (int y = 16; y < H; y += 16) {
    fb_fill_rect(1, y, 6, 1, COL_TEXT);
    fb_fill_rect(W - 7, y, 6, 1, COL_TEXT);
    snprintf(label, sizeof label, "%03d", y);
    print_at(W - 32, y - 8, COL_LABEL, label);
  }
  fb_fill_rect(1, (int)(benchFrames % (H - 2)) + 1, W - 2, 1, COL_TITLE);
}

void bench_frame(void) {
  char line[48];

  if (benchSetCopyMode) {
    set_copy_mode((uint8_t)((benchSetCopyMode & 0xFFu) - 1u), (uint8_t)(benchSetCopyMode >> 8));
    benchSetCopyMode = 0;
  }
  if (benchSetCopyLines) {
    set_copy_lines((uint16_t)benchSetCopyLines);
    benchSetCopyLines = 0;
  }
  if (benchSetSoundPath) {
    s_bench_prefer_ym = benchSetSoundPath == 2u;
    audio_prefer_ym(s_bench_prefer_ym);
    benchSetSoundPath = 0;
  }
  if (bench_logic_poll()) palette_set(s_palette); /* the game had the screen */
  if (bench_logic_holds_screen()) return;           /* the framebuffer is the card's buffer */
  bench_scene_poll();
  if (bench_scene_active()) {
    bench_scene_frame();
    benchFrames++;
    return;
  }
  if (benchStill) {
    s_still = true;
    palette_set(benchStillPalette);
    fb_publish();
    benchFrames++;
    return;
  }
  if (s_still) {
    s_still = false;
    palette_set(s_palette);
  }

  font_set_font(&font8x8);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);
  draw_test_card();

  print_at(16, 16, COL_TITLE, "CHO REN SHA BENCH");

  if (st_session_hellos() == 0) {
    print_at(16, 40, COL_TEXT, "NO HELLO YET");
  } else {
    uint16_t tos = st_session_tos_version();
    snprintf(line, sizeof line, "%s TOS %X.%02X %s",
             machine_name(st_session_machine()), (unsigned)(tos >> 8),
             (unsigned)(tos & 0xFFu),
             (st_session_features() & CART_ST_FEATURE_BLITTER) ? "BLITTER"
                                                                 : "");
    print_at(16, 40, COL_TEXT, line);
  }

  snprintf(line, sizeof line, "FRAMES %lu %lu/S", (unsigned long)benchFrames,
           (unsigned long)benchFramesPerSecond);
  print_at(16, 56, COL_TEXT, line);
  snprintf(line, sizeof line, "C2P %lu US",
           (unsigned long)fb_last_convert_us());
  print_at(16, 72, COL_TEXT, line);
  snprintf(line, sizeof line, "IKBD %lu OVR %lu", (unsigned long)ikbdBytes,
           (unsigned long)ikbd_overruns());
  print_at(144, 72, COL_TEXT, line);
  snprintf(line, sizeof line, "COPY %s %s %u", k_copy_modes[s_copy_mode],
           fbLayoutNatural ? "BLITTER" : "MOVEM",
           (unsigned)(s_copy_piece ? s_copy_piece : CART_BLIT_PIECE_DEFAULT));
  print_at(16, 88, COL_TEXT, line);

  /* What the ST reported, frame by frame: both borders open, one, none. */
  print_at(16, 96, COL_LABEL, "BORDERS");
  snprintf(line, sizeof line, "BOTH %lu",
           (unsigned long)fbBorderFrames[CART_BORDER_TOP | CART_BORDER_BOTTOM]);
  print_at(16, 112, COL_TEXT, line);
  snprintf(line, sizeof line, "TOP %lu BOTTOM %lu",
           (unsigned long)fbBorderFrames[CART_BORDER_TOP],
           (unsigned long)fbBorderFrames[CART_BORDER_BOTTOM]);
  print_at(16, 128, COL_TEXT, line);
  snprintf(line, sizeof line, "NONE %lu", (unsigned long)fbBorderFrames[0]);
  print_at(16, 144, COL_TEXT, line);

  print_at(16, 224, COL_LABEL, s_st_silent ? "F5: SOUND ON NEXT BOOT"
                                            : "F5: NO SOUND NEXT BOOT");
  print_at(208, 224, COL_LABEL, s_tone_step ? "F6: TONE" : "F6: -");
  print_at(16, 208, COL_LABEL, "F1-F4: INPUT F7: COPY F8: PIECE");
  const bench_logic_result_t *logic = bench_logic_result();
  if (logic->state == BENCH_LOGIC_DONE || logic->state == BENCH_LOGIC_ERROR) {
    snprintf(line, sizeof line, "LOGIC %lu FR %lu/%lu US",
             (unsigned long)logic->frames,
             (unsigned long)(logic->frames ? logic->logic_sum / logic->frames : 0),
             (unsigned long)logic->logic_max);
    print_at(16, 160, COL_TEXT, line);
    snprintf(line, sizeof line, "CHECKS %lu BAD %lu %s",
             (unsigned long)logic->checkpoints, (unsigned long)logic->mismatches,
             logic->state == BENCH_LOGIC_ERROR ? "ERROR" : "");
    print_at(16, 176, COL_TEXT, line);
  } else if (logic->state == BENCH_LOGIC_NO_TRANSLATION) {
    print_at(16, 160, COL_TEXT, "LOGIC: NO TRANSLATION");
  }
  /* A build with the translated logic has no scene (bench_scene.c). */
  print_at(16, 192, COL_LABEL, bench_logic_available() ? "INSERT: LOGIC" : "HELP: SCENE");
  print_at(16, 240, COL_LABEL, "ESC: GEM");

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
  switch (commandId) {
    case DEVHOOKS_APP_BENCH_PING:
      return benchFrames + 1u;
    case DEVHOOKS_APP_BENCH_SILENT:
      set_silent(payloadSize >= 2u && payload[0] != 0u);
      return 1;
    case DEVHOOKS_APP_BENCH_TONE:
      set_tone(payloadSize >= 2u ? payload[0] : 0u);
      return 1;
    case DEVHOOKS_APP_BENCH_STILL:
      benchStill = payloadSize >= 2u && payload[0] != 0u;
      DPRINTF("bench: still %s\n", benchStill ? "on" : "off");
      return 1;
    case DEVHOOKS_APP_BENCH_COPY_LINES:
      if (payloadSize < 2u) return 0;
      set_copy_lines(payload[0]);
      return 1;
    case DEVHOOKS_APP_BENCH_COPY_MODE: {
      uint16_t piece = payloadSize >= 4u ? payload[1] : 0u;
      if (payloadSize < 2u || payload[0] > CART_BLIT_MODE_BLITTER ||
          piece > 255u) {
        return 0;
      }
      set_copy_mode((uint8_t)payload[0], (uint8_t)piece);
      return 1;
    }
    default:
      return 0;
  }
}
