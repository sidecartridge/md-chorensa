/**
 * File: bench_logic.c
 * Description: The bench's logic run and the translated game's runtime on
 *              the RP. See bench_logic.h.
 *
 * The runtime is the host's (tools/xlat/host/xl_host.c) cut to what the game
 * calls after its stage load: the frame wait (XSP_VSYNC) runs the game's VBL
 * handler and records the checkpoints, XSP_OUT takes the sprite list, and
 * the IOCS calls the attract demo makes answer as the host answers them. Any
 * other call stops the run with its name and address. The translated code,
 * its tables and the game's addresses come from the store's images
 * (tools/xlat/xl_store.h).
 */

#include "bench_logic.h"

#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "pico/time.h"

#ifdef XL_STORE

#include <setjmp.h>

#include "aconfig.h"
#include "audio.h"
#include "cart_shared.h"
#include "constants.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "ikbd.h"
#include "select.h"
#include "palette.h"
#include "sdcard.h"
#include "st_session.h"
#include "store.h"
#include "xl.h"
#include "xl_render.h"

#define LAST_FRAME 6000u
#define HISTOGRAM_BINS 256u /* the logic times, 32 us a bin: up to 8 ms */
#define HISTOGRAM_SHIFT 5u
#define CHECKPOINT_INTERVAL 60u
#define CHECKPOINT_BYTES 32u
#define MAXIMUM_SPRITES 512u
#define FRAME_TIMER_HZ100 5546u
#define PATH_BYTES 96u

extern const char release_build_id[]; /* build_id.c */
/* The pages the game writes: this array, then the cartridge window's free
 * arena (rp_image.py points the stage's page table at both, by these
 * symbols; xl_store.h). */
uint8_t xl_rp_ram[XL_STORE_RAM_BYTES] __attribute__((aligned(4)));
/* The text plane in its layout (xl_store.h's XL_STORE_TEXT_*): the stage's
 * page table points its pages into it. */
uint8_t xl_rp_text[XL_STORE_TEXT_BYTES] __attribute__((aligned(4)));
#define POOL_RAM_PAGES (XL_STORE_RAM_BYTES >> XL_PAGE_BITS)
#define POOL_PAGES (POOL_RAM_PAGES + (XL_STORE_WINDOW_FREE_BYTES >> XL_PAGE_BITS))
_Static_assert(XL_STORE_WINDOW_FREE == CART_APP_FREE_OFFSET &&
                   XL_STORE_WINDOW_FREE + XL_STORE_WINDOW_FREE_BYTES <= CART_FRAMEBUFFER_OFFSET,
               "the game's pages in the window stay inside its free arena");

xl_cpu_t xl;
uint8_t *xl_mem;             /* the host's flat memory: unused (XL_PAGED) */
uint8_t **xl_regions[256];   /* the page table, from the stage's image */

static const xl_store_header_t *s_common, *s_stage;
static jmp_buf s_exit;
static uint32_t s_frame_count, s_total_vbl, s_frame_sprite_count, s_last_sprite_count;
static uint32_t s_effect_end_tick;
static int s_samples_decoded;
static uint32_t s_sprite_list_count;
static uint32_t s_frame_end_us; /* when the last frame wait ended */

/* The play mode (benchPlay): the frames drawn and shown as the game runs. */
static int s_playing;
static xl_render_frame_t s_frame;
static uint16_t s_palette[PALETTE_ENTRIES];
volatile uint32_t benchPlay = 1;  /* the host writes 0: back to the bench */
/* The host's hold: the play stops once it has shown the frame the host's
 * screens number benchPlayHoldAt (their frame wait starts at that count),
 * says so in benchPlayHeld, and goes on when the host clears the hold. */
volatile uint32_t benchPlayHoldAt;
volatile uint32_t benchPlayHeld;
bench_play_t benchPlayStats;

/* Live input (xlat.py's patches): from the first touch of joystick 1, its
 * fire, the cursor keys or the space bar, the player flies the demo's ship;
 * each lap starts as the demo again. The stick as _JOYGET reports it to the
 * player's reads (closed switches: bits 0-3 up, down, left, right, bit 5
 * button A, bit 6 button B). */
#define STICK_UP 0x01u
#define STICK_DOWN 0x02u
#define STICK_LEFT 0x04u
#define STICK_RIGHT 0x08u
#define STICK_SHOT 0x20u  /* button A: the fire button */
#define STICK_BOMB 0x40u  /* button B: the space bar */
#define SC_UP 0x48u
#define SC_DOWN 0x50u
#define SC_LEFT 0x4Bu
#define SC_RIGHT 0x4Du
#define SC_SPACE 0x39u
static int s_live, s_live_read;
static uint8_t s_keys;  /* the stick's bits the keyboard holds */
static uint8_t s_stick;

int xl_live_input(void) { return s_live; }
void xl_live_read(int on) { s_live_read = on; }

/* The game's variables the runtime uses, from the common image's header. */
#define GAME(name) (s_common->layout[XL_LAYOUT_##name])

/* The frames by their logic time, 32 us a bin (the last one: 8 ms or more),
 * readable over SWD (tools/dev/swd.py logic). */
uint32_t benchLogicHistogram[HISTOGRAM_BINS];
/* The first checkpoint that differed: the RP's, then the host's. */
uint8_t benchLogicMismatch[2 * CHECKPOINT_BYTES];

/* The host's writes to the card (bench_logic.h). */
typedef struct {
  volatile uint32_t hold;    /* the host: the bench stops drawing */
  volatile uint32_t held;    /* the bench: it has */
  volatile uint32_t command; /* the host: 1 writes the chunk; the bench clears it */
  uint32_t offset, length;
  int32_t status;
  char path[PATH_BYTES];     /* in the app's folder */
} bench_sd_t;
bench_sd_t benchSd;
#endif

bench_logic_result_t benchLogicResult;
volatile uint32_t benchLogicRequest; /* the host writes 1: a run */

#ifdef XL_STORE

static void stop(int state, const char *what, uint32_t address) {
  benchLogicResult.state = (uint32_t)state;
  benchLogicResult.error_address = address;
  strncpy(benchLogicResult.error, what, sizeof benchLogicResult.error - 1u);
  longjmp(s_exit, 1);
}

void xl_fatal(const char *what, uint32_t address) { stop(BENCH_LOGIC_ERROR, what, address); }
void xl_divide_by_zero(uint32_t pc) { xl_fatal("divide by zero", pc); }
void xl_stop(uint32_t sr, uint32_t pc) {
  (void)sr;
  xl_fatal("STOP", pc);
}

static const xl_store_entry_t *find(const xl_store_header_t *h, uint32_t address) {
  uint32_t lo = 0, hi = h->entry_count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2u;
    if (h->entries[mid].address < address) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return lo < h->entry_count && h->entries[lo].address == address ? &h->entries[lo] : NULL;
}

uint32_t xl_call(uint32_t address) {
  address &= 0xFFFFFFu;
  const xl_store_entry_t *e = find(s_common, address);
  if (e == NULL) e = find(s_stage, address);
  if (e == NULL) xl_fatal("no translated code", address);
  return e->fn();
}

uint32_t xl_call_stage(uint32_t address) {
  const xl_store_entry_t *e = find(s_stage, address);
  if (e == NULL) xl_fatal("not in the stage's image", address);
  return e->fn();
}

uint32_t xl_get_sr(void) {
  return (xl.sr_high << 8) | (xl.x << 4) | (xl.n << 3) | (xl.z << 2) | (xl.v << 1) | xl.c;
}

void xl_set_sr(uint32_t v) {
  xl.sr_high = (v >> 8) & 0xFFu;
  xl.x = (v >> 4) & 1u;
  xl.n = (v >> 3) & 1u;
  xl.z = (v >> 2) & 1u;
  xl.v = (v >> 1) & 1u;
  xl.c = v & 1u;
}

uint32_t xl_hw_rd(uint32_t a, int size) {
  return size == 1 ? rd8(a) : size == 2 ? rd16(a) : rd32(a);
}

void xl_hw_wr(uint32_t a, uint32_t v, int size) {
  if (size == 1) {
    wr8(a, v);
  } else if (size == 2) {
    wr16(a, v);
  } else {
    wr32(a, v);
  }
}

/* The runtime the store's images call, kept in the link: nothing in the
 * firmware calls most of it, and the images resolve it by this firmware's
 * symbols (rp_store.py links them against its ELF). */
const void *const xl_runtime[] __attribute__((used)) = {
    (const void *)xl_call,          (const void *)xl_call_stage,
    (const void *)xl_fatal,         (const void *)xl_divide_by_zero,
    (const void *)xl_stop,          (const void *)xl_get_sr,
    (const void *)xl_set_sr,        (const void *)xl_hw_rd,
    (const void *)xl_hw_wr,         (const void *)xl_trap,
    (const void *)xl_linef,         (const void *)xl_patch_xsp_vsync,
    (const void *)xl_patch_xsp_out, (const void *)xl_patch_clear_text_planes,
    (const void *)xl_patch_wait_vbl, (const void *)xl_patch_sincos,
    (const void *)xl_patch_samples_loaded, (const void *)xl_live_input,
    (const void *)xl_live_read,     (const void *)&xl,
    (const void *)xl_regions,       (const void *)&xl_mem,
};

/* --- Frames --------------------------------------------------------------- */

static void run_vbl(void) {
  s_total_vbl++;
  uint32_t handler = rd32(0x118);
  if (handler == 0) return;
  A(7) -= 4;
  wr32(A(7), 0x00FFFFF0u);
  A(7) -= 2;
  wr16(A(7), xl_get_sr());
  uint32_t r = xl_call(handler);
  if (r != 0x00FFFFF0u) xl_fatal("the VBL handler returned elsewhere", r);
}

static void read_bytes(uint8_t *out, uint32_t a, uint32_t n) {
  for (uint32_t k = 0; k < n; k++) out[k] = (uint8_t)rd8(a + k);
}

/* The host's checkpoint (xl_host.c frame_wait_end), against the host's. */
static void checkpoint(void) {
  uint8_t c[CHECKPOINT_BYTES];
  memset(c, 0, sizeof c);
  c[0] = (uint8_t)(s_frame_count >> 24);
  c[1] = (uint8_t)(s_frame_count >> 16);
  c[2] = (uint8_t)(s_frame_count >> 8);
  c[3] = (uint8_t)s_frame_count;
  read_bytes(c + 4, GAME(PLAYER_SCORE), 4);
  read_bytes(c + 8, GAME(WORD_00088E6C), 2);
  read_bytes(c + 10, GAME(BACKGROUND_SCROLL_COUNTER), 2);
  c[12] = (uint8_t)(s_last_sprite_count >> 8);
  c[13] = (uint8_t)s_last_sprite_count;
  read_bytes(c + 16, GAME(PLAYER_INFO_STRUCT), 14);
  if (s_live) return; /* the player's game is not the host's */
  uint32_t k = s_frame_count / CHECKPOINT_INTERVAL - 1u;
  benchLogicResult.checkpoints++;
  if (k >= s_stage->checkpoint_count) return;
  const uint8_t *expected = s_stage->checkpoints + k * CHECKPOINT_BYTES;
  if (memcmp(c, expected, CHECKPOINT_BYTES) != 0) {
    if (benchLogicResult.mismatches++ == 0) {
      benchLogicResult.first_mismatch = s_frame_count;
      memcpy(benchLogicMismatch, c, CHECKPOINT_BYTES);
      memcpy(benchLogicMismatch + CHECKPOINT_BYTES, expected, CHECKPOINT_BYTES);
    }
  }
}

/* The bottom half of the frame, on core 1. */
static void band_job(void *arg) {
  xl_render_band(fb_chunked_buffer, (const xl_render_frame_t *)arg, XL_RENDER_H / 2,
                 XL_RENDER_H);
}

static void note(uint32_t *sum, uint32_t *max, uint32_t us) {
  *sum += us;
  if (us > *max) *max = us;
}

/* Play mode: the frame the game has just finished, drawn on both cores and
 * shown (fb_publish waits for the ST to have copied the last one: one
 * game frame a VBL), with what the main loop would do meanwhile. */
static void present(uint32_t game_us) {
  uint32_t t0 = time_us_32();
  s_frame.art = s_stage->art;
  xl_render_prepare(&s_frame);
  fb_core1_dispatch(band_job, &s_frame);
  xl_render_band(fb_chunked_buffer, &s_frame, 0, XL_RENDER_H / 2);
  fb_core1_wait();
  if (xl_render_palette(s_stage->art, s_palette)) palette_set(s_palette);
  uint32_t t1 = time_us_32();
  fb_publish();
  uint32_t t2 = time_us_32();
  audio_render_frame(); /* the music and the effects: song_samples */
  uint32_t t3 = time_us_32();
  ikbd_pump();
  select_poll();
  ikbd_key_event_t key;
  while (ikbd_pop_key(&key)) {
    uint8_t bit = key.scancode == SC_UP      ? STICK_UP
                  : key.scancode == SC_DOWN  ? STICK_DOWN
                  : key.scancode == SC_LEFT  ? STICK_LEFT
                  : key.scancode == SC_RIGHT ? STICK_RIGHT
                  : key.scancode == SC_SPACE ? STICK_BOMB
                                             : 0u;
    s_keys = key.is_press ? (uint8_t)(s_keys | bit) : (uint8_t)(s_keys & ~bit);
  }
  ikbd_joystick_t stick;
  ikbd_read_joystick(1, &stick);
  s_stick = (uint8_t)(s_keys | ((stick.state & IKBD_JOY_UP) ? STICK_UP : 0u) |
                      ((stick.state & IKBD_JOY_DOWN) ? STICK_DOWN : 0u) |
                      ((stick.state & IKBD_JOY_LEFT) ? STICK_LEFT : 0u) |
                      ((stick.state & IKBD_JOY_RIGHT) ? STICK_RIGHT : 0u) |
                      ((stick.state & IKBD_JOY_FIRE) ? STICK_SHOT : 0u));
  if (s_stick) s_live = 1;
  if (benchPlayHoldAt && s_frame_count == benchPlayHoldAt + 1u) {
    benchPlayHeld = benchPlayHoldAt;
    while (benchPlayHoldAt == benchPlayHeld && benchPlay) {
      fb_pump_rom3();
      ikbd_pump();
      select_poll();
    }
    benchPlayHeld = 0;
  }
  benchPlayStats.frames++;
  note(&benchPlayStats.game_sum, &benchPlayStats.game_max, game_us);
  note(&benchPlayStats.draw_sum, &benchPlayStats.draw_max, t1 - t0);
  note(&benchPlayStats.publish_sum, &benchPlayStats.publish_max, t2 - t1);
  note(&benchPlayStats.audio_sum, &benchPlayStats.audio_max, t3 - t2);
  if (!benchPlay) stop(BENCH_LOGIC_DONE, "stopped", 0);
}

/* XSP_VSYNC: the game's frame ends here. Its logic time is from the end of
 * the last wait; then its VBLs (the handler timed apart) and the checkpoint. */
void xl_patch_xsp_vsync(void) {
  uint32_t start = time_us_32();
  uint32_t logic = start - s_frame_end_us;
  uint32_t played = benchLogicResult.frames;
  uint32_t bin = logic >> HISTOGRAM_SHIFT;
  benchLogicHistogram[bin < HISTOGRAM_BINS ? bin : HISTOGRAM_BINS - 1u]++;
  benchLogicResult.frames = played + 1u;
  benchLogicResult.logic_sum += logic;
  if (logic > benchLogicResult.logic_max) benchLogicResult.logic_max = logic;

  uint32_t vbls = D(0) & 0xFFFFu;
  s_last_sprite_count = s_frame_sprite_count;
  s_frame_sprite_count = 0;
  for (uint32_t k = 0; k < vbls; k++) run_vbl();
  s_frame_count++;
  uint32_t counter = rd16(GAME(VBL_VWAIT_COUNTER));
  D(0) = (D(0) & 0xFFFF0000u) | counter;
  wr16(GAME(VBL_VWAIT_COUNTER), 0);
  xl.n = 0;
  xl.z = 1;
  xl.v = 0;
  xl.c = 0;

  uint32_t end = time_us_32();
  uint32_t vbl = end - start;
  benchLogicResult.vbl_sum += vbl;
  if (vbl > benchLogicResult.vbl_max) benchLogicResult.vbl_max = vbl;
  if (s_playing) present(logic + vbl);
  if (s_frame_count % CHECKPOINT_INTERVAL == 0) checkpoint();
  if (s_frame_count >= LAST_FRAME) stop(BENCH_LOGIC_DONE, "", 0);
  s_frame_end_us = time_us_32();
}

void xl_patch_wait_vbl(void) {
  uint32_t ticks = rd16(A(7) + 6);
  D(1) = (D(1) & 0xFFFF0000u) | ticks;
  s_last_sprite_count = s_frame_sprite_count;
  s_frame_sprite_count = 0;
  if (!(ticks & 0x8000u)) {
    for (uint32_t k = 0; k <= ticks; k++) run_vbl();
  }
  s_frame_count++;
  if (s_frame_count % CHECKPOINT_INTERVAL == 0) checkpoint();
  if (s_frame_count >= LAST_FRAME) stop(BENCH_LOGIC_DONE, "", 0);
}

void xl_patch_xsp_out(void) {
  uint32_t count = (rd32(GAME(CURRENT_SPRITE_DATA_ENTRY)) - GAME(SPRITE_DATA_TABLE)) / 8u;
  if (count > MAXIMUM_SPRITES) count = MAXIMUM_SPRITES;
  s_frame_sprite_count = count;
  s_sprite_list_count = count;
  if (s_playing) xl_render_take_sprites(&s_frame, GAME(SPRITE_DATA_TABLE), count);
  wr32(GAME(CURRENT_SPRITE_DATA_ENTRY), GAME(SPRITE_DATA_TABLE));
  D(0) = 0;
  xl.n = 0;
  xl.z = 1;
  xl.v = 0;
  xl.c = 0;
}

void xl_patch_clear_text_planes(void) { memset(xl_rp_text, 0, sizeof xl_rp_text); }

void xl_patch_sincos(void) { xl_fatal("the sine tables load", GAME(SIN_COS_TABLES_START)); }
void xl_patch_samples_loaded(void) { xl_fatal("the samples load", 0); }

/* --- IOCS, as the host answers the attract demo ---------------------------- */

static const uint32_t k_adpcm_rates_hz100[5] = {4000000u * 100u / 1024u, 4000000u * 100u / 768u,
                                                8000000u * 100u / 1024u, 8000000u * 100u / 768u,
                                                8000000u * 100u / 512u};

static void effect_start(uint32_t address, uint32_t bytes, uint32_t mode);
static void effect_stop(void);

static void iocs(uint32_t pc) {
  switch (D(0) & 0xFFu) {
    case 0x04: /* _BITSNS: no key held */
      D(0) = 0;
      return;
    case 0x3B: /* _JOYGET: nothing pressed, but to the player's reads */
      D(0) = s_live_read ? (0xFFFFFF00u | (~(uint32_t)s_stick & 0xFFu)) : 0xFFFFFFFFu;
      return;
    case 0x60: { /* _ADPCMOUT: d1 = rate << 8 | channels, d2 = bytes, a1 = the sample */
      uint32_t channels = D(1) & 3u, length = D(2);
      if (s_samples_decoded && channels && (length >> 1)) {
        uint32_t rate = (D(1) & 0xFFFFu) >> 8;
        if (rate > 4) rate = 4;
        s_effect_end_tick = length * FRAME_TIMER_HZ100 / k_adpcm_rates_hz100[rate] + 1u + s_total_vbl;
        if (s_playing) effect_start(A(1) & 0xFFFFFFu, length, D(1) & 0xFFFFu);
      }
      D(0) = 0;
      return;
    }
    case 0x66: /* _ADPCMSNS */
      D(0) = (s_effect_end_tick && s_total_vbl < s_effect_end_tick) ? 2u : 0u;
      return;
    case 0x67: /* _ADPCMMOD */
      if (D(1) == 0) {
        s_effect_end_tick = 0;
        effect_stop();
      }
      D(0) = 0;
      return;
    case 0x81: /* _B_SUPER */
      D(0) = 0xFFFFFFFFu;
      return;
    default:
      xl_fatal("an IOCS call the bench does not serve", pc);
  }
}

/* --- The music: MCDRV (trap #4), the songs from the card ------------------ */

/* In play mode the game's MCDRV calls play the songs the host converted
 * (tools/xlat/music_convert.py: in the app's folder on the card, signed
 * 8-bit mono, intro then loop; NAME.ST8 at the DMA chip's rate, NAME.YM8 at
 * the YM's, filtered for it), the one for the output the ST uses, at the
 * rate its header gives, read from the card as the template's PCM output
 * asks for samples (audio_render_frame, once a frame). A fade lowers the song by MCDRV's 64 steps over speed x the
 * song's unit / 10,000 s, then stops it, as the Amiga port measured. */
#define MCDRV_TRANSMDC 0x01u
#define MCDRV_PLAYMUSIC 0x02u
#define MCDRV_STOPMUSIC 0x05u
#define MCDRV_FADEOUT 0x14u
#define SONG_HEADER 32u
#define NO_SONG 0xFFFFFFFFu
static const uint8_t k_fade_volumes[64] = {
    64, 59, 54, 49, 45, 42, 38, 35, 32, 29, 27, 25, 23, 21, 19, 18, 16, 15, 14, 12, 11, 10,
    10, 9,  8,  7,  7,  6,  6,  5,  5,  4,  4,  4,  3,  3,  3,  3,  2,  2,  2,  2,  2,  2,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0};
static void card_path(char *out, const char *name);
static FIL s_song_file; /* static: a FIL does not belong on core 0's stack */
static struct {
  int open, playing;
  uint32_t transferred;          /* the song MCDRV would play next */
  uint32_t intro, loop, pos;     /* in samples */
  uint32_t level, fade_unit;
  uint32_t rate;                 /* the file's, in Hz */
  uint32_t fade_at, fade_length; /* samples into the fade and its length; 0: none */
} s_song = {.transferred = NO_SONG};

static void song_samples(int8_t *buf, uint32_t samples);
static uint32_t s_out_rate; /* the rate the PCM callback was given, in Hz */

/* The effects: _ADPCMOUT plays a sample of the bank the host decoded
 * (tools/xlat/host/xl_host.c's effects.bin, EFFECTS.SE8 in the app's folder
 * on the card: 'CRSE', the address the game's table points from, the bytes,
 * then signed 8-bit samples), one at a time, a start replacing the one
 * playing (the game decides by priority, PLAY_PCM_SAMPLE), _ADPCMMOD 0
 * stopping it. Read from the card as the song is, a sector at a time at the
 * file's sector boundaries (FatFs reads those straight into the chunk,
 * leaving the volume's buffer, which the song's reads share, alone), and
 * added to the song, stepped from its ADPCM rate to the callback's, the sum
 * saturated. */
#define EFFECTS_HEADER 16u
#define EFFECT_CHUNK 512u /* a sector */
static FIL s_effects_file;
static struct {
  int open;
  uint32_t base, size;
  uint32_t at, end;              /* the playing effect's bytes in the bank; at >= end: none */
  uint32_t frac, rate_hz100;     /* the fraction past `at` (16 bits), its ADPCM rate */
  uint32_t chunk_at, chunk_len;  /* the file's bytes in `chunk`: from offset chunk_at */
  int8_t chunk[EFFECT_CHUNK];
} s_fx;

static void effects_open(void) {
  char path[PATH_BYTES];
  uint8_t head[EFFECTS_HEADER];
  UINT got = 0;
  s_fx.open = 0;
  s_fx.at = s_fx.end = 0;
  s_fx.chunk_len = 0;
  card_path(path, "EFFECTS.SE8");
  if (f_open(&s_effects_file, path, FA_READ) != FR_OK) return;
  if (f_read(&s_effects_file, head, sizeof head, &got) != FR_OK || got != sizeof head ||
      memcmp(head, "CRSE", 4) != 0) {
    f_close(&s_effects_file);
    return;
  }
  s_fx.base = (uint32_t)head[4] | (uint32_t)head[5] << 8 | (uint32_t)head[6] << 16 |
              (uint32_t)head[7] << 24;
  s_fx.size = (uint32_t)head[8] | (uint32_t)head[9] << 8 | (uint32_t)head[10] << 16 |
              (uint32_t)head[11] << 24;
  s_fx.open = 1;
}

static void effects_close(void) {
  if (s_fx.open) f_close(&s_effects_file);
  s_fx.open = 0;
}

static void effect_start(uint32_t address, uint32_t bytes, uint32_t mode) {
  uint32_t rate = mode >> 8;
  if (!s_fx.open || address < s_fx.base || address - s_fx.base >= s_fx.size) return;
  s_fx.at = address - s_fx.base;
  s_fx.end = s_fx.at + bytes < s_fx.size ? s_fx.at + bytes : s_fx.size;
  s_fx.frac = 0;
  s_fx.rate_hz100 = k_adpcm_rates_hz100[rate > 4u ? 4u : rate];
}

static void effect_stop(void) { s_fx.end = s_fx.at; }

/* Add the playing effect to `samples` of the song. */
static void effect_mix(int8_t *buf, uint32_t samples) {
  if (s_fx.at >= s_fx.end || s_out_rate == 0) return;
  uint32_t step = (uint32_t)(((uint64_t)s_fx.rate_hz100 << 16) / (s_out_rate * 100u));
  for (uint32_t k = 0; k < samples && s_fx.at < s_fx.end; k++) {
    uint32_t file_at = EFFECTS_HEADER + s_fx.at;
    if (file_at < s_fx.chunk_at || file_at - s_fx.chunk_at >= s_fx.chunk_len) {
      uint32_t sector = file_at & ~(EFFECT_CHUNK - 1u);
      UINT got = 0;
      if ((f_tell(&s_effects_file) != sector && f_lseek(&s_effects_file, sector) != FR_OK) ||
          f_read(&s_effects_file, s_fx.chunk, EFFECT_CHUNK, &got) != FR_OK ||
          sector + got <= file_at) {
        s_fx.end = s_fx.at;
        s_fx.chunk_len = 0;
        break;
      }
      s_fx.chunk_at = sector;
      s_fx.chunk_len = got;
    }
    int32_t m = (int32_t)buf[k] + s_fx.chunk[file_at - s_fx.chunk_at];
    buf[k] = (int8_t)(m > 127 ? 127 : m < -128 ? -128 : m);
    s_fx.frac += step;
    s_fx.at += s_fx.frac >> 16;
    s_fx.frac &= 0xFFFFu;
  }
}

static void song_stop(void) {
  s_song.playing = 0;
  if (s_song.open) f_close(&s_song_file);
  s_song.open = 0;
}

/* Play song `index` of the common image's table from `frames` game frames
 * (50 a second) in. */
static void song_play(uint32_t index, uint32_t frames) {
  song_stop();
  if (index >= s_common->song_count) return;
  char name[24], path[PATH_BYTES];
  snprintf(name, sizeof name, audio_uses_dma() ? "%.12s.ST8" : "%.12s.YM8",
           s_common->songs[index].name);
  card_path(path, name);
  uint8_t head[SONG_HEADER];
  UINT got = 0;
  if (f_open(&s_song_file, path, FA_READ) != FR_OK) return;
  s_song.open = 1;
  if (f_read(&s_song_file, head, sizeof head, &got) != FR_OK || got != sizeof head ||
      memcmp(head, "CRS8", 4) != 0) {
    song_stop();
    return;
  }
  s_song.rate = (uint32_t)head[4] | (uint32_t)head[5] << 8 | (uint32_t)head[6] << 16 |
                (uint32_t)head[7] << 24;
  if (s_song.rate == 0) {
    song_stop();
    return;
  }
  s_song.intro = (uint32_t)head[8] | (uint32_t)head[9] << 8 | (uint32_t)head[10] << 16 |
                 (uint32_t)head[11] << 24;
  s_song.loop = (uint32_t)head[12] | (uint32_t)head[13] << 8 | (uint32_t)head[14] << 16 |
                (uint32_t)head[15] << 24;
  s_song.fade_unit = (uint32_t)head[16] | (uint32_t)head[17] << 8;
  s_song.level = (uint32_t)head[18] | (uint32_t)head[19] << 8;
  if (s_song.level == 0 || s_song.level > 64u) s_song.level = 64;
  uint32_t skip = (uint32_t)((uint64_t)frames * s_song.rate / 50u);
  s_song.pos = skip < s_song.intro + s_song.loop ? skip : 0;
  f_lseek(&s_song_file, SONG_HEADER + s_song.pos);
  s_song.fade_length = 0;
  s_song.playing = 1;
  s_out_rate = s_song.rate;
  audio_set_pcm_callback(song_samples, s_out_rate);
}

/* The template's PCM output asks for `samples` (from audio_render_frame). */
static void song_samples(int8_t *buf, uint32_t samples) {
  uint32_t done = 0;
  while (done < samples && s_song.playing) {
    uint32_t end = s_song.intro + s_song.loop;
    uint32_t want = samples - done;
    if (want > end - s_song.pos) want = end - s_song.pos;
    UINT got = 0;
    if (f_read(&s_song_file, buf + done, want, &got) != FR_OK || got == 0) {
      song_stop();
      break;
    }
    done += got;
    s_song.pos += got;
    if (s_song.pos >= end) {
      if (s_song.loop == 0) {
        song_stop();
      } else {
        s_song.pos = s_song.intro;
        f_lseek(&s_song_file, SONG_HEADER + s_song.pos);
      }
    }
  }
  memset(buf + done, 0, samples - done);
  for (uint32_t k = 0; k < done; k++) {
    uint32_t volume = 64;
    if (s_song.fade_length) {
      uint32_t step = (uint32_t)(((uint64_t)(s_song.fade_at + k) * 64u) / s_song.fade_length);
      volume = step < 64u ? k_fade_volumes[step] : 0u;
    }
    buf[k] = (int8_t)((int32_t)buf[k] * (int32_t)(volume * s_song.level) / (64 * 64));
  }
  if (s_song.fade_length) {
    s_song.fade_at += done;
    if (s_song.fade_at >= s_song.fade_length) song_stop();
  }
  effect_mix(buf, samples);
}

/* MCDRV's calls the game makes: d0.l the function, d1.l, a0. */
static void mcdrv(void) {
  uint32_t f = D(0);
  if (s_playing) {
    if (f == MCDRV_TRANSMDC) {
      s_song.transferred = NO_SONG;
      for (uint32_t k = 0; k < s_common->song_count; k++) {
        if (s_common->songs[k].address == (A(0) & 0xFFFFFFu)) s_song.transferred = k;
      }
    } else if (f == MCDRV_PLAYMUSIC) {
      song_play(s_song.transferred, 0);
    } else if (f == MCDRV_STOPMUSIC) {
      song_stop();
    } else if (f == MCDRV_FADEOUT && s_song.playing) {
      s_song.fade_at = 0;
      s_song.fade_length = (uint32_t)((uint64_t)D(1) * s_song.fade_unit * s_song.rate / 10000u);
      if (s_song.fade_length == 0) s_song.fade_length = 1;
    }
  }
  D(0) = 0;
}

void xl_trap(uint32_t n, uint32_t pc) {
  if (n == 15) {
    iocs(pc);
  } else if (n == 4) {
    mcdrv();
  } else if (n != 2) {
    xl_fatal("TRAP", pc);
  }
}

void xl_linef(uint32_t word, uint32_t pc) {
  (void)word;
  xl_fatal("a DOS or FLOAT call", pc);
}

/* --- The run --------------------------------------------------------------- */

/* The app's folder on the card, and a file in it. */
static void card_path(char *out, const char *name) {
  SettingsConfigEntry *folder = settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  snprintf(out, PATH_BYTES, "%s/%s", folder ? folder->value : "", name);
}

/* Between flash operations: the bus, the IKBD and SELECT keep going. */
static void pump(void) {
  fb_pump_rom3();
  ikbd_pump();
  select_poll();
}

static const xl_store_header_t *load(int k, const char *name, uint32_t base, uint32_t size) {
  char path[PATH_BYTES];
  card_path(path, name);
  uint32_t t0 = time_us_32();
  int status = store_load(path, base, size, release_build_id, pump);
  benchLogicResult.load_status[k] = (uint32_t)status;
  benchLogicResult.load_us[k] = time_us_32() - t0;
  DPRINTF("logic: %s %d in %lu us\n", path, status, (unsigned long)benchLogicResult.load_us[k]);
  if (status != STORE_COPIED && status != STORE_SKIPPED) return NULL;
  const xl_store_header_t *h = store_header(base);
  if (h && strncmp(h->firmware, release_build_id, XL_STORE_FIRMWARE_ID - 1u) != 0) return NULL;
  return h;
}

/* The pool's page k, in rp_image.py's order. */
static uint8_t *pool_page(uint32_t k) {
  if (k < POOL_RAM_PAGES) return xl_rp_ram + (k << XL_PAGE_BITS);
  return (uint8_t *)__rom_in_ram_start__ + XL_STORE_WINDOW_FREE +
         ((k - POOL_RAM_PAGES) << XL_PAGE_BITS);
}

static void restore(void) {
  if (s_playing) {
    s_song.transferred = s_stage->song_playing;
    song_play(s_stage->song_playing, s_stage->song_frames);
  }
  s_live = 0;
  s_live_read = 0;
  for (int r = 0; r < 256; r++) xl_regions[r] = (uint8_t **)s_stage->regions[r];
  for (uint32_t k = 0; k < s_stage->ram_pages; k++) {
    memcpy(pool_page(k), s_stage->ram_init + (k << XL_PAGE_BITS), 1u << XL_PAGE_BITS);
  }
  memset(pool_page(s_stage->ram_pages), 0, 1u << XL_PAGE_BITS); /* the spare */
  memset(xl_rp_text, 0, sizeof xl_rp_text);
  for (uint32_t k = 0; k < s_stage->text_row_count; k++) {
    memcpy(xl_rp_text + s_stage->text_rows[k], s_stage->text_init + 32u * k, 32u);
  }
  const uint32_t *w = s_stage->state + 2;
  for (int k = 0; k < 8; k++) xl.d[k] = *w++;
  for (int k = 0; k < 8; k++) xl.a[k] = *w++;
  xl.n = *w++;
  xl.z = *w++;
  xl.v = *w++;
  xl.c = *w++;
  xl.x = *w++;
  xl.sr_high = *w++;
  s_frame_count = *w++;
  s_total_vbl = *w++;
  s_frame_sprite_count = *w++;
  s_last_sprite_count = *w++;
  s_effect_end_tick = *w++;
  s_samples_decoded = (int)*w++;
  s_sprite_list_count = *w++;
}

bool bench_logic_available(void) { return true; }

/* Load the images and play from the snapshot to LAST_FRAME: timed (the
 * logic bench), or drawn and shown, over and over until the host says
 * otherwise (play). */
static void run(int drawn);

void bench_logic_run(void) { run(0); }

static void run(int drawn) {
  memset(&benchLogicResult, 0, sizeof benchLogicResult);
  memset(benchLogicHistogram, 0, sizeof benchLogicHistogram);
  /* Called from the main loop, which joins every core 1 job before it goes
   * on (fb_chunked.c): core 1 is parked in RAM, the flash may be written. */
  benchLogicResult.state = BENCH_LOGIC_LOADING;
  s_common = load(0, "COMMON.IMG", XL_STORE_COMMON, XL_STORE_COMMON_SIZE);
  s_stage = load(1, "STAGE1.IMG", XL_STORE_STAGE, XL_STORE_STAGE_SIZE);
  storeForceCopy = 0;
  benchLogicResult.state = BENCH_LOGIC_IDLE;
  const char *problem = NULL;
  if (s_common == NULL || s_stage == NULL || s_stage->state == NULL) {
    problem = "no images for this firmware";
  } else if (s_stage->ram_pages + 1u > POOL_PAGES) {
    problem = "the RAM pages do not fit";
  }
  if (problem) {
    benchLogicResult.state = BENCH_LOGIC_ERROR;
    strncpy(benchLogicResult.error, problem, sizeof benchLogicResult.error - 1u);
    return;
  }
  if (drawn && s_stage->art == NULL) {
    benchLogicResult.state = BENCH_LOGIC_ERROR;
    strncpy(benchLogicResult.error, "the stage has no art", sizeof benchLogicResult.error - 1u);
    return;
  }
  uint32_t begin = time_us_32();
  s_playing = drawn;
  if (drawn) {
    /* The output's rate until a song gives its own: the songs' (ST8 or YM8). */
    s_out_rate = audio_uses_dma() ? AUDIO_DMA_RATE_HZ : PROFILE_YM_RATE_HZ;
    audio_set_pcm_callback(song_samples, s_out_rate);
    effects_open();
  }
  do {
    restore();
    benchLogicResult.first_frame = s_frame_count;
    benchLogicResult.state = BENCH_LOGIC_IDLE;
    DPRINTF("logic: from frame %lu%s\n", (unsigned long)s_frame_count, drawn ? ", playing" : "");
    s_frame_end_us = time_us_32();
    if (setjmp(s_exit) == 0) {
      uint32_t r = xl_call(s_stage->resume);
      for (;;) r = xl_call(r);
    }
    if (drawn) benchPlayStats.laps++;
  } while (drawn && benchPlay && benchLogicResult.state == BENCH_LOGIC_DONE);
  if (drawn) {
    song_stop();
    effects_close();
    audio_set_pcm_callback(NULL, AUDIO_DMA_RATE_HZ);
  }
  s_playing = 0;
  benchLogicResult.total_us = time_us_32() - begin;
  DPRINTF("logic: %lu frames, %lu us mean, %lu max, %lu/%lu checkpoints differ (%s)\n",
          (unsigned long)benchLogicResult.frames,
          (unsigned long)(benchLogicResult.frames
                              ? benchLogicResult.logic_sum / benchLogicResult.frames
                              : 0),
          (unsigned long)benchLogicResult.logic_max, (unsigned long)benchLogicResult.mismatches,
          (unsigned long)benchLogicResult.checkpoints, benchLogicResult.error);
}

/* The host's writes to the card, between frames. */
static void serve_card(void) {
  if (!benchSd.hold) {
    benchSd.held = 0;
    return;
  }
  benchSd.held = 1;
  if (!benchSd.command) return;
  char path[PATH_BYTES];
  benchSd.path[PATH_BYTES - 1u] = '\0';
  card_path(path, benchSd.path);
  uint32_t length = benchSd.length <= FB_CHUNKED_SIZE ? benchSd.length : 0;
  benchSd.status = store_put(path, benchSd.offset, fb_chunked_buffer, length);
  benchSd.command = 0;
}

bool bench_logic_holds_screen(void) { return benchSd.held != 0; }

/* The game streams its music and effects from the card beside the frames:
 * on a card whose SPI runs slower than this it does not start (measured, a
 * good card holds 50 frames a second down to 3 MHz; this leaves room for
 * poor ones). */
#define PLAY_MIN_SD_HZ 8000000u

extern const struct FB_FONT font8x8; /* fb.c */

static void say(int y, unsigned color, const char *text) {
  font_set_color(color);
  font_move(16u, (unsigned)y);
  font_print(text);
}

/* The card is too slow: the ST says so, and at a key press the RP restarts
 * into Booster, where the user raises the SD card's speed. The host can
 * stop the play over SWD instead (benchPlay 0), to leave without Booster. */
static void refuse_slow_card(uint32_t hz) {
  static const uint16_t k_colours[PALETTE_ENTRIES] = {[0] = 0x000, [7] = 0x555, [14] = 0x770,
                                                      [15] = 0x777};
  char line[40];
  benchLogicResult.state = BENCH_LOGIC_ERROR;
  snprintf(benchLogicResult.error, sizeof benchLogicResult.error,
           "the card's clock is %lu kHz, under %lu", (unsigned long)(hz / 1000u),
           (unsigned long)(PLAY_MIN_SD_HZ / 1000u));
  palette_set(k_colours);
  fb_chunked_clear(0);
  font_set_font(&font8x8);
  say(48, 14, "MICROSD CARD TOO SLOW");
  snprintf(line, sizeof line, "THE CARD RUNS AT %lu.%lu MHZ.", (unsigned long)(hz / 1000000u),
           (unsigned long)(hz / 100000u % 10u));
  say(80, 15, line);
  say(92, 15, "THE GAME NEEDS 8 MHZ OR MORE.");
  say(124, 15, "PRESS A KEY: BOOSTER STARTS.");
  say(136, 15, "IN BOOSTER, SET THE SD CARD");
  say(148, 15, "SPEED TO 8 MHZ OR MORE,");
  say(160, 15, "OR USE A FASTER CARD.");
  fb_publish();
  ikbd_set_input_mode(IKBD_INPUT_KEYBOARD);
  ikbd_key_event_t key;
  while (ikbd_pop_key(&key)) {
  }
  while (benchPlay) {
    fb_pump_rom3();
    ikbd_pump();
    select_poll();
    if (ikbd_pop_key(&key) && key.is_press) st_session_return_to_booster();
  }
}

/* Play until the host stops it or the run fails (then not again until the
 * host asks: benchPlay back to 1). */
static void play(void) {
  uint32_t hz = sdcard_getSpiHz();
  if (hz < PLAY_MIN_SD_HZ) {
    refuse_slow_card(hz);
    return;
  }
  ikbd_set_input_mode(IKBD_INPUT_JOYSTICKS); /* stick 1 for the player */
  run(1);
  ikbd_set_input_mode(IKBD_INPUT_KEYBOARD);
  if (benchLogicResult.state != BENCH_LOGIC_DONE) benchPlay = 0;
}

#else /* no translation in this build */

bool bench_logic_available(void) { return false; }
bool bench_logic_holds_screen(void) { return false; }
static void serve_card(void) {}
static void play(void) {}
volatile uint32_t benchPlay;

void bench_logic_run(void) {
  memset(&benchLogicResult, 0, sizeof benchLogicResult);
  benchLogicResult.state = BENCH_LOGIC_NO_TRANSLATION;
}

#endif

bool bench_logic_poll(void) {
  serve_card();
  if (benchLogicRequest) {
    benchLogicRequest = 0;
    bench_logic_run();
    return true;
  }
  if (benchPlay && bench_logic_available() && !bench_logic_holds_screen()) {
    play();
    return true;
  }
  return false;
}

const bench_logic_result_t *bench_logic_result(void) { return &benchLogicResult; }
