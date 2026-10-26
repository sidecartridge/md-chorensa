/**
 * File: bench_logic.c
 * Description: The bench's logic run and the translated game's runtime on
 *              the RP. See bench_logic.h.
 *
 * The runtime is the host's (tools/xlat/host/xl_host.c) cut to what the game
 * calls after its stage load: the frame wait (XSP_VSYNC) runs the game's VBL
 * handler and records the checkpoints, XSP_OUT takes the sprite list, and
 * the IOCS calls the attract demo makes answer as the host answers them. Any
 * other call stops the run with its name and address.
 */

#include "bench_logic.h"

#include <string.h>

#include "debug.h"
#include "pico/time.h"

#ifdef XL_RP

#include <setjmp.h>

#include "fb_chunked.h"
#include "xl.h"
#include "xl_functions.h"
#include "xl_layout.h"
#include "xl_rp_image.h"

#define LAST_FRAME 6000u
#define MAX_FRAMES 5400u
#define CHECKPOINT_INTERVAL 60u
#define CHECKPOINT_BYTES 32u
#define MAXIMUM_SPRITES 512u
#define FRAME_TIMER_HZ100 5546u

_Static_assert(XL_RP_RAM_PAGES * (1u << XL_PAGE_BITS) <= FB_CHUNKED_SIZE,
               "the game's RAM pages do not fit the borrowed framebuffer");

xl_cpu_t xl;
uint8_t *xl_mem; /* the host's flat memory: unused (XL_PAGED) */

typedef uint32_t (*xl_fn_t)(void);
extern const struct {
  uint32_t address;
  xl_fn_t fn;
} xl_entries[];
extern const uint32_t xl_entry_count;

static jmp_buf s_exit;
static uint32_t s_frame_count, s_total_vbl, s_frame_sprite_count, s_last_sprite_count;
static uint32_t s_effect_end_tick;
static int s_samples_decoded;
static uint32_t s_sprite_list_count;
static uint32_t s_frame_end_us; /* when the last frame wait ended */

/* The logic time of every frame, readable over SWD (tools/dev/swd.py logic). */
uint16_t benchLogicUs[MAX_FRAMES];
/* The first checkpoint that differed: the RP's, then the host's. */
uint8_t benchLogicMismatch[2 * CHECKPOINT_BYTES];
#endif

bench_logic_result_t benchLogicResult;
volatile uint32_t benchLogicRequest; /* the host writes 1: a run */

#ifdef XL_RP

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

uint32_t xl_call(uint32_t address) {
  address &= 0xFFFFFFu;
  uint32_t lo = 0, hi = xl_entry_count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2u;
    if (xl_entries[mid].address < address) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  if (lo >= xl_entry_count || xl_entries[lo].address != address) {
    xl_fatal("no translated code", address);
  }
  return xl_entries[lo].fn();
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
  read_bytes(c + 4, XL_PLAYER_SCORE, 4);
  read_bytes(c + 8, XL_WORD_00088E6C, 2);
  read_bytes(c + 10, XL_BACKGROUND_SCROLL_COUNTER, 2);
  c[12] = (uint8_t)(s_last_sprite_count >> 8);
  c[13] = (uint8_t)s_last_sprite_count;
  read_bytes(c + 16, XL_PLAYER_INFO_STRUCT, 14);
  uint32_t k = s_frame_count / CHECKPOINT_INTERVAL - 1u;
  benchLogicResult.checkpoints++;
  if (k >= XL_RP_CHECKPOINTS) return;
  const uint8_t *expected = xl_rp_checkpoints + k * CHECKPOINT_BYTES;
  if (memcmp(c, expected, CHECKPOINT_BYTES) != 0) {
    if (benchLogicResult.mismatches++ == 0) {
      benchLogicResult.first_mismatch = s_frame_count;
      memcpy(benchLogicMismatch, c, CHECKPOINT_BYTES);
      memcpy(benchLogicMismatch + CHECKPOINT_BYTES, expected, CHECKPOINT_BYTES);
    }
  }
}

/* XSP_VSYNC: the game's frame ends here. Its logic time is from the end of
 * the last wait; then its VBLs (the handler timed apart) and the checkpoint. */
void xl_patch_xsp_vsync(void) {
  uint32_t start = time_us_32();
  uint32_t logic = start - s_frame_end_us;
  uint32_t played = benchLogicResult.frames;
  if (played < MAX_FRAMES) benchLogicUs[played] = (uint16_t)(logic > 0xFFFFu ? 0xFFFFu : logic);
  benchLogicResult.frames = played + 1u;
  benchLogicResult.logic_sum += logic;
  if (logic > benchLogicResult.logic_max) benchLogicResult.logic_max = logic;

  uint32_t vbls = D(0) & 0xFFFFu;
  s_last_sprite_count = s_frame_sprite_count;
  s_frame_sprite_count = 0;
  for (uint32_t k = 0; k < vbls; k++) run_vbl();
  s_frame_count++;
  uint32_t counter = rd16(XL_VBL_VWAIT_COUNTER);
  D(0) = (D(0) & 0xFFFF0000u) | counter;
  wr16(XL_VBL_VWAIT_COUNTER, 0);
  xl.n = 0;
  xl.z = 1;
  xl.v = 0;
  xl.c = 0;

  uint32_t end = time_us_32();
  uint32_t vbl = end - start;
  benchLogicResult.vbl_sum += vbl;
  if (vbl > benchLogicResult.vbl_max) benchLogicResult.vbl_max = vbl;
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
  uint32_t count = (rd32(XL_CURRENT_SPRITE_DATA_ENTRY) - XL_SPRITE_DATA_TABLE) / 8u;
  if (count > MAXIMUM_SPRITES) count = MAXIMUM_SPRITES;
  s_frame_sprite_count = count;
  s_sprite_list_count = count;
  wr32(XL_CURRENT_SPRITE_DATA_ENTRY, XL_SPRITE_DATA_TABLE);
  D(0) = 0;
  xl.n = 0;
  xl.z = 1;
  xl.v = 0;
  xl.c = 0;
}

void xl_patch_clear_text_planes(void) {
  for (uint32_t a = 0xE00000u; a < 0xE80000u; a += 4) wr32(a, 0);
}

void xl_patch_sincos(void) { xl_fatal("the sine tables load", XL_SIN_COS_TABLES_START); }
void xl_patch_samples_loaded(void) { xl_fatal("the samples load", 0); }

/* --- IOCS, as the host answers the attract demo ---------------------------- */

static const uint32_t k_adpcm_rates_hz100[5] = {4000000u * 100u / 1024u, 4000000u * 100u / 768u,
                                                8000000u * 100u / 1024u, 8000000u * 100u / 768u,
                                                8000000u * 100u / 512u};

static void iocs(uint32_t pc) {
  switch (D(0) & 0xFFu) {
    case 0x04: /* _BITSNS: no key held */
      D(0) = 0;
      return;
    case 0x3B: /* _JOYGET: nothing pressed */
      D(0) = 0xFFFFFFFFu;
      return;
    case 0x60: { /* _ADPCMOUT */
      uint32_t channels = D(1) & 3u, length = D(2);
      if (s_samples_decoded && channels && (length >> 1)) {
        uint32_t rate = (D(1) & 0xFFFFu) >> 8;
        if (rate > 4) rate = 4;
        s_effect_end_tick = length * FRAME_TIMER_HZ100 / k_adpcm_rates_hz100[rate] + 1u + s_total_vbl;
      }
      D(0) = 0;
      return;
    }
    case 0x66: /* _ADPCMSNS */
      D(0) = (s_effect_end_tick && s_total_vbl < s_effect_end_tick) ? 2u : 0u;
      return;
    case 0x67: /* _ADPCMMOD */
      if (D(1) == 0) s_effect_end_tick = 0;
      D(0) = 0;
      return;
    case 0x81: /* _B_SUPER */
      D(0) = 0xFFFFFFFFu;
      return;
    default:
      xl_fatal("an IOCS call the bench does not serve", pc);
  }
}

void xl_trap(uint32_t n, uint32_t pc) {
  if (n == 15) {
    iocs(pc);
  } else if (n != 2 && n != 4) {
    xl_fatal("TRAP", pc);
  }
}

void xl_linef(uint32_t word, uint32_t pc) {
  (void)word;
  xl_fatal("a DOS or FLOAT call", pc);
}

/* --- The run --------------------------------------------------------------- */

static void restore(void) {
  memcpy(xl_rp_ram, xl_rp_ram_init, XL_RP_WRITTEN_PAGES << XL_PAGE_BITS);
  memset(xl_rp_ram + (XL_RP_WRITTEN_PAGES << XL_PAGE_BITS), 0, 1u << XL_PAGE_BITS);
  const uint32_t *w = xl_rp_state + 2;
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

void bench_logic_run(void) {
  memset(&benchLogicResult, 0, sizeof benchLogicResult);
  restore();
  benchLogicResult.first_frame = s_frame_count;
  DPRINTF("logic: from frame %lu\n", (unsigned long)s_frame_count);
  uint32_t begin = time_us_32();
  s_frame_end_us = begin;
  if (setjmp(s_exit) == 0) {
    uint32_t r = xl_call(xl_rp_state[1]);
    for (;;) r = xl_call(r);
  }
  benchLogicResult.total_us = time_us_32() - begin;
  DPRINTF("logic: %lu frames, %lu us mean, %lu max, %lu/%lu checkpoints differ (%s)\n",
          (unsigned long)benchLogicResult.frames,
          (unsigned long)(benchLogicResult.frames
                              ? benchLogicResult.logic_sum / benchLogicResult.frames
                              : 0),
          (unsigned long)benchLogicResult.logic_max, (unsigned long)benchLogicResult.mismatches,
          (unsigned long)benchLogicResult.checkpoints, benchLogicResult.error);
}

#else /* no translation in this build */

bool bench_logic_available(void) { return false; }

void bench_logic_run(void) {
  memset(&benchLogicResult, 0, sizeof benchLogicResult);
  benchLogicResult.state = BENCH_LOGIC_NO_TRANSLATION;
}

#endif

void bench_logic_poll(void) {
  if (benchLogicRequest) {
    benchLogicRequest = 0;
    bench_logic_run();
  }
}

const bench_logic_result_t *bench_logic_result(void) { return &benchLogicResult; }
