/**
 * File: bench_scene.c
 * Description: The bench's scene and its sweep. See bench_scene.h.
 */

#include "bench_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_scene_assets.h"
#include "debug.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "palette.h"
#include "pico/time.h"
#include "scene_draw.h"

extern const struct FB_FONT font8x8; /* fb.c */

/* The attract demo's 95th percentile and maximum of sprites a frame. */
#define SPRITES_P95 169u
#define SPRITES_MAX 268u
/* The patterns a RAM cache holds (4 bits: 128 bytes each, 32 KB). */
#define RAM_PATTERNS 256u
/* Frames of each run not counted while the caches warm up. */
#define WARMUP_FRAMES 8u
/* Page 1 scrolls a line a frame, page 0 four (the game's parallax). */
#define PAGE1_SPEED 1u
#define PAGE0_SPEED 4u

enum { BG_RUNS, BG_KEY };
enum { SPR_KEY8, SPR_NIB4, SPR_RLE };

typedef struct {
  uint8_t background;
  uint8_t sprites;
  uint8_t ram;   /* patterns from the RAM cache */
  uint8_t cores;
  uint16_t patterns;
  const char *name; /* nine characters at most: the results table */
} scene_config_t;

static const scene_config_t k_configs[] = {
    {BG_RUNS, SPR_KEY8, 0, 1, SCENE_PATTERNS, "RUN K8"},
    {BG_KEY, SPR_KEY8, 0, 1, SCENE_PATTERNS, "KEY K8"},
    {BG_RUNS, SPR_NIB4, 0, 1, SCENE_PATTERNS, "RUN N4"},
    {BG_RUNS, SPR_RLE, 0, 1, SCENE_PATTERNS, "RUN RLE"},
    {BG_RUNS, SPR_NIB4, 0, 1, RAM_PATTERNS, "N4 256"},
    {BG_RUNS, SPR_NIB4, 1, 1, RAM_PATTERNS, "N4 RAM"},
    {BG_RUNS, SPR_KEY8, 0, 2, SCENE_PATTERNS, "K8 2C"},
    {BG_RUNS, SPR_NIB4, 0, 2, SCENE_PATTERNS, "N4 2C"},
    {BG_RUNS, SPR_RLE, 0, 2, SCENE_PATTERNS, "RLE 2C"},
    {BG_RUNS, SPR_NIB4, 1, 2, RAM_PATTERNS, "N4RAM 2C"},
};
#define CONFIGS (sizeof k_configs / sizeof k_configs[0])
#define RUNS (2u * CONFIGS)

/* Readable and writable over SWD by their symbols. */
bench_scene_result_t benchSceneResults[RUNS];
volatile uint32_t benchSceneRequest; /* the host's frames a run: starts a sweep */
uint32_t benchSceneRun;              /* the run in progress; RUNS when done */

typedef struct {
  int16_t x, y;
  uint16_t pattern;
  uint8_t palette;
} scene_sprite_t;

enum { IDLE, SWEEP, RESULTS };
static int s_state = IDLE;
static uint32_t s_frames_a_run;
static uint32_t s_frame_in_run;
static uint32_t s_t; /* the scene's clock: frames drawn */
static uint32_t s_last_publish_us;
static scene_sprite_t s_sprites[SPRITES_MAX];
static uint32_t s_sprite_count;
static uint8_t s_luts[16][16];
static uint8_t *s_ram; /* the RAM cache, during a sweep */

typedef struct {
  int y0, y1;
} band_t;
static band_t s_band1 = {128, 256};

void bench_scene_init(void) {
  scene_draw_init();
  /* The 16 palettes: colour 0 transparent, the others rotated. */
  for (int p = 0; p < 16; p++) {
    for (int c = 0; c < 16; c++) {
      s_luts[p][c] = (uint8_t)(c ? 1 + (c - 1 + p) % 15 : 0);
    }
  }
  benchSceneRun = RUNS;
}

static const scene_config_t *config(void) { return &k_configs[benchSceneRun / 2u]; }

static uint32_t hash(uint32_t i) {
  i ^= i >> 16;
  i *= 0x7FEB352Du;
  i ^= i >> 15;
  i *= 0x846CA68Bu;
  i ^= i >> 16;
  return i;
}

/* The sprites of frame s_t: each from its own place and speed, moving, the
 * screen's 256 + 16 on each side wrapped so that they cross every edge. */
/* a mod span, from 0 to span - 1 whatever a's sign. */
static int wrap(int a, int span) {
  int m = a % span;
  return m < 0 ? m + span : m;
}

static void place_sprites(uint32_t count, uint32_t patterns) {
  const int span = SCENE_W + SCENE_SPRITE * 2;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t h = hash(i + 1u);
    int vx = (int)((h >> 18) % 5u) - 2, vy = (int)((h >> 21) % 7u) - 3;
    if (vx == 0 && vy == 0) vy = 2;
    int x = wrap((int)(h % (uint32_t)span) + vx * (int)s_t, span);
    int y = wrap((int)((h >> 9) % (uint32_t)span) + vy * (int)s_t, span);
    s_sprites[i].x = (int16_t)(x - SCENE_SPRITE);
    s_sprites[i].y = (int16_t)(y - SCENE_SPRITE);
    s_sprites[i].pattern = (uint16_t)(hash(h) % patterns);
    s_sprites[i].palette = (uint8_t)(h >> 28);
  }
  s_sprite_count = count;
}

static void __not_in_flash_func(draw_band)(int y0, int y1) {
  const scene_config_t *c = config();
  uint8_t *fb = fb_chunked_buffer;
  const int lines = SCENE_PAGE_LINES;
  int scroll1 = (int)((lines * 1000u - PAGE1_SPEED * s_t) % (uint32_t)lines);
  int scroll0 = (int)((lines * 1000u - PAGE0_SPEED * s_t) % (uint32_t)lines);
  scene_page4(fb, scene_page1_4, lines, scroll1, y0, y1);
  if (c->background == BG_RUNS) {
    scene_page_runs(fb, scene_page0_runs, scene_page0_index, lines, scroll0, y0, y1);
  } else {
    scene_page4_key(fb, scene_page0_4, lines, scroll0, y0, y1);
  }
  struct FB_BITMAP bm = {SCENE_SPRITE, SCENE_SPRITE, NULL};
  const uint8_t *patterns4 = c->ram ? s_ram : scene_patterns_4;
  for (uint32_t i = 0; i < s_sprite_count; i++) {
    const scene_sprite_t *s = &s_sprites[i];
    if (s->y + SCENE_SPRITE <= y0 || s->y >= y1) continue;
    switch (c->sprites) {
      case SPR_KEY8:
        bm.data = scene_patterns_8 + s->pattern * SCENE_SPRITE * SCENE_SPRITE;
        fb_blit_key_band(&bm, s->x, s->y, 0, y0, y1);
        break;
      case SPR_NIB4:
        scene_sprite4(fb, patterns4 + s->pattern * (SCENE_SPRITE * SCENE_SPRITE / 2),
                      s_luts[s->palette], s->x, s->y, y0, y1);
        break;
      default:
        scene_sprite_rle(fb, scene_patterns_rle + scene_patterns_rle_index[s->pattern],
                         s->x, s->y, y0, y1);
        break;
    }
  }
}

static void __not_in_flash_func(band_job)(void *arg) {
  band_t *b = (band_t *)arg;
  draw_band(b->y0, b->y1);
}

static void begin_run(void) {
  memset(&benchSceneResults[benchSceneRun], 0, sizeof benchSceneResults[0]);
  benchSceneResults[benchSceneRun].config = benchSceneRun / 2u;
  benchSceneResults[benchSceneRun].sprites = (benchSceneRun & 1u) ? SPRITES_MAX : SPRITES_P95;
  s_frame_in_run = 0;
}

void bench_scene_start(uint32_t frames) {
  s_frames_a_run = frames ? frames : BENCH_SCENE_FRAMES_DEFAULT;
  memset(benchSceneResults, 0, sizeof benchSceneResults);
  if (s_ram == NULL) {
    s_ram = malloc(RAM_PATTERNS * SCENE_SPRITE * SCENE_SPRITE / 2u);
    if (s_ram != NULL) {
      memcpy(s_ram, scene_patterns_4, RAM_PATTERNS * SCENE_SPRITE * SCENE_SPRITE / 2u);
    }
  }
  benchSceneRun = 0;
  begin_run();
  s_state = SWEEP;
  s_t = 0;
  s_last_publish_us = time_us_32();
  palette_set(scene_palette);
  DPRINTF("scene: sweep of %lu frames a run, %u runs%s\n", (unsigned long)s_frames_a_run,
          (unsigned)RUNS, s_ram ? "" : " (no RAM for the cache: its runs skipped)");
}

void bench_scene_stop(void) {
  s_state = IDLE;
  benchSceneRun = RUNS;
  free(s_ram);
  s_ram = NULL;
}

bool bench_scene_active(void) { return s_state != IDLE; }

void bench_scene_poll(void) {
  uint32_t request = benchSceneRequest;
  if (request) {
    benchSceneRequest = 0;
    bench_scene_start(request);
  }
}

static void next_run(void) {
  benchSceneRun++;
  /* No RAM for the cache: skip the runs that need it (left at 0 frames). */
  while (benchSceneRun < RUNS && config()->ram && s_ram == NULL) benchSceneRun++;
  if (benchSceneRun >= RUNS) {
    s_state = RESULTS;
    free(s_ram);
    s_ram = NULL;
    DPRINTF("scene: sweep done\n");
    return;
  }
  begin_run();
}

static void sweep_frame(void) {
  const scene_config_t *c = config();
  uint32_t count = (benchSceneRun & 1u) ? SPRITES_MAX : SPRITES_P95;
  uint32_t t0 = time_us_32();
  place_sprites(count, c->patterns);
  if (c->cores == 2u) {
    fb_core1_dispatch(band_job, &s_band1);
    draw_band(0, s_band1.y0);
    fb_core1_wait();
  } else {
    draw_band(0, SCENE_W);
  }
  /* The score line, on the text plane in the game. */
  char line[40];
  snprintf(line, sizeof line, "SC %08lu %-9s %3lu", (unsigned long)(s_t * 10u), c->name,
           (unsigned long)count);
  font_set_font(&font8x8);
  font_set_color(15);
  font_move(0, 0);
  font_print(line);
  uint32_t draw = time_us_32() - t0;

  fb_publish();
  uint32_t convert = fb_last_convert_us();
  uint32_t now = time_us_32();
  uint32_t period = now - s_last_publish_us;
  s_last_publish_us = now;
  s_t++;

  bench_scene_result_t *r = &benchSceneResults[benchSceneRun];
  if (s_frame_in_run >= WARMUP_FRAMES) {
    r->frames++;
    r->draw_sum += draw;
    r->convert_sum += convert;
    r->period_sum += period;
    if (draw > r->draw_max) r->draw_max = draw;
    if (convert > r->convert_max) r->convert_max = convert;
    if (period > r->period_max) r->period_max = period;
  }
  if (++s_frame_in_run >= WARMUP_FRAMES + s_frames_a_run) next_run();
}

/* Tenths of a millisecond, as "dd.d". */
static void ms(char *out, size_t size, uint32_t us) {
  snprintf(out, size, "%2lu.%lu", (unsigned long)(us / 1000u),
           (unsigned long)(us % 1000u / 100u));
}

static void results_frame(void) {
  static const uint16_t k_text_palette[16] = {0x000, 0x777, 0x775, 0x557, 0x777, 0x777, 0x777, 0x777,
                                              0x777, 0x777, 0x777, 0x777, 0x777, 0x777, 0x777, 0x777};
  palette_set(k_text_palette);
  fb_chunked_clear(0);
  font_set_font(&font8x8);
  font_set_color(2);
  font_move(0, 0);
  font_print("SCENE: MS MEAN AND MAX");
  font_set_color(3);
  font_move(0, 12);
  font_print("CONFIG   SPR DRAW  MAX  C2P FPS");
  for (uint32_t i = 0; i < RUNS; i++) {
    const bench_scene_result_t *r = &benchSceneResults[i];
    char line[48], d[8], dm[8], cv[8];
    if (r->frames == 0u) {
      snprintf(line, sizeof line, "%-9s%3lu   -", k_configs[i / 2u].name,
               (unsigned long)((i & 1u) ? SPRITES_MAX : SPRITES_P95));
    } else {
      ms(d, sizeof d, r->draw_sum / r->frames);
      ms(dm, sizeof dm, r->draw_max);
      ms(cv, sizeof cv, r->convert_sum / r->frames);
      uint32_t fps = (uint32_t)(1000000ull * r->frames / (r->period_sum ? r->period_sum : 1u));
      snprintf(line, sizeof line, "%-9s%3lu %s %s %s %2lu", k_configs[i / 2u].name,
               (unsigned long)r->sprites, d, dm, cv, (unsigned long)fps);
    }
    font_set_color(1);
    font_move(0, (unsigned)(26 + 11 * i));
    font_print(line);
  }
  font_set_color(3);
  font_move(0, 248);
  font_print("HELP: AGAIN  UNDO: CARD");
  fb_publish();
}

void bench_scene_frame(void) {
  if (s_state == SWEEP) {
    sweep_frame();
  } else {
    results_frame();
  }
}
