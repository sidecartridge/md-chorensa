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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "hardware/dma.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
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
#include "install.h"
#include "select.h"
#include "palette.h"
#include "sdcard.h"
#include "st_session.h"
#include "store.h"
#include "xl.h"
#include "xl_render.h"
#include "zipread.h"

/* The game starts at the title, in the menus' image, which holds the state
 * the host left after the boot. */
#define START_IMAGE "MENUS.IMG"
#define HISTOGRAM_BINS 256u /* the logic times, 32 us a bin: up to 8 ms */
#define HISTOGRAM_SHIFT 5u
#define CHECKPOINT_INTERVAL XL_STORE_CHECK_FRAMES
#define CHECKPOINT_BYTES XL_STORE_CHECK_BYTES
#define MAXIMUM_SPRITES 512u
#define FRAME_TIMER_HZ100 5546u
#define PATH_BYTES 96u

extern const char release_build_id[]; /* build_id.c */
/* The pages the game writes: the chunky buffer's part a banded frame
 * leaves, this array, then the cartridge window's free arena (rp_image.py
 * points the stage's page table at them, by these symbols; xl_store.h). */
uint8_t xl_rp_ram[XL_STORE_RAM_BYTES] __attribute__((aligned(4)));
_Static_assert(XL_STORE_RAM_BYTES >= ZIP_WORK_BYTES, "the install gate's work in the game's RAM");
_Static_assert(XL_STORE_FB_FREE_BYTES >= INSTALL_SONG_WORK_BYTES,
               "the install gate's song buffers past the chunky buffer's banded part");
/* The text plane in its layout (xl_store.h's XL_STORE_TEXT_*): the stage's
 * page table points its pages into it. */
uint8_t xl_rp_text[XL_STORE_TEXT_BYTES] __attribute__((aligned(4)));
#define POOL_FB_PAGES (XL_STORE_FB_FREE_BYTES >> XL_PAGE_BITS)
#define POOL_RAM_PAGES (XL_STORE_RAM_BYTES >> XL_PAGE_BITS)
#define POOL_PAGES (POOL_FB_PAGES + POOL_RAM_PAGES + (XL_STORE_WINDOW_FREE_BYTES >> XL_PAGE_BITS))
_Static_assert(XL_STORE_WINDOW_FREE == CART_APP_FREE_OFFSET &&
                   XL_STORE_WINDOW_FREE + XL_STORE_WINDOW_FREE_BYTES <= CART_FRAMEBUFFER_OFFSET,
               "the game's pages in the window stay inside its free arena");
_Static_assert(XL_STORE_FB_FREE == FB_BANDED_BYTES &&
                   XL_STORE_FB_FREE + XL_STORE_FB_FREE_BYTES == FB_CHUNKED_SIZE,
               "the game's pages in the chunky buffer are the part a banded frame leaves");

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
bench_hold_times_t benchHoldTimes;

/* A script for stick 1 on the card (XL_STORE_INPUT_FILE, rp_store.py
 * --input), when the start file says so: the game played as the host's
 * XL_INPUT plays it (tools/xlat/host/xl_host.c input_scripted), so the
 * bench compares a mortal game's checkpoints with the host's. Lines "FRAME
 * MASK" in frame order (hex: 1 up, 2 down, 4 left, 8 right, 20 button A, 40
 * button B), the stick from that frame on, or "auto", the host's scripted
 * player; '#' starts a comment; the host reads 256 steps at most. Read from
 * the card a step at a time as the frames reach it. A frame that reads the
 * stick more than 64 times lets the buttons go, as the host's does. */
#define INPUT_STEPS 256u
#define INPUT_AUTO 0xFFFFFFFFu
static int s_scripted; /* the start file's XL_STORE_START_SCRIPTED */
static void card_path(char *out, const char *name);
#define INPUT_LINE 64u
static FIL s_input_file; /* static: a FIL does not belong on core 0's stack */
static struct {
  int on, ahead;           /* the script is open; the next step is read */
  uint32_t steps;          /* read so far */
  uint32_t mask;           /* the stick's now */
  uint32_t next_frame, next_mask;
  uint32_t reads_frame, reads;
} s_input;

static void input_close(void) {
  if (s_input.on) f_close(&s_input_file);
  s_input.on = 0;
}

/* The script's next step into next_frame and next_mask; 0 at its end. */
static int input_step(void) {
  char line[INPUT_LINE];
  while (s_input.steps < INPUT_STEPS) {
    uint32_t n = 0;
    UINT got = 0;
    char c = 0;
    while (f_read(&s_input_file, &c, 1, &got) == FR_OK && got == 1 && c != '\n') {
      if (n < INPUT_LINE - 1u) line[n++] = c;
    }
    if (n == 0 && got == 0) return 0;
    line[n] = 0;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    unsigned frame;
    char word[16];
    if (sscanf(line, "%u %15s", &frame, word) != 2) continue;
    s_input.next_frame = frame;
    s_input.next_mask = strcmp(word, "auto") == 0 ? INPUT_AUTO
                                                  : (uint32_t)strtoul(word, NULL, 16) & 0xFFu;
    s_input.steps++;
    return 1;
  }
  return 0;
}

static void input_open(void) {
  char path[PATH_BYTES];
  input_close();
  memset(&s_input, 0, sizeof s_input);
  s_input.reads_frame = 0xFFFFFFFFu;
  if (!s_scripted) return;
  card_path(path, XL_STORE_INPUT_FILE);
  if (f_open(&s_input_file, path, FA_READ) != FR_OK) return;
  s_input.on = 1;
  s_input.ahead = input_step();
}

/* The host's scripted player (xl_host.c scripted_stick): left and right a
 * second each, the shot held, the bomb a moment every six seconds. */
static uint32_t scripted_stick(void) {
  uint32_t closed = ((s_frame_count / 55u) & 1u) ? 0x08u : 0x04u;
  closed |= 0x20u;
  if (s_frame_count % 330u < 3u) closed |= 0x40u;
  return 0xFFFFFF00u | (~closed & 0xFFu);
}

static uint32_t input_joyget(void) {
  while (s_input.ahead && s_input.next_frame <= s_frame_count) {
    s_input.mask = s_input.next_mask;
    s_input.ahead = input_step();
  }
  s_input.reads = s_frame_count == s_input.reads_frame ? s_input.reads + 1u : 0u;
  s_input.reads_frame = s_frame_count;
  uint32_t joyget = s_input.mask == INPUT_AUTO ? scripted_stick()
                                               : (0xFFFFFF00u | (~s_input.mask & 0xFFu));
  if (s_input.reads > 64u) joyget |= 0x60u;
  return joyget;
}

/* The player: joystick 1, the cursor keys and the space bar, as _JOYGET
 * reports them to every read of the game's, as on the X68000 (closed
 * switches: bits 0-3 up, down, left, right, bit 5 button A, bit 6 button
 * B): a press ends the attract demo, fire on the title's START starts a
 * game. The demo's replay is not taken over (xlat.py's live-input patches
 * stay off on the RP). From the first touch the game is the player's, not
 * the host's: its checkpoints are no longer compared. */
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
#define SC_ALT 0x38u /* the shot, for a player without a joystick */
/* The keys the game reads as the X68000's (IOCS _BITSNS: a byte of 8 key
 * codes a group): ESC its pause, 1, TAB, SHIFT and CTRL the hidden debug
 * mode's. While the game waits in its pause, G leaves to GEM and B to
 * Booster. On the title K opens the hidden menu (xl_kakusi_key). */
#define SC_ESC 0x01u
#define SC_1 0x02u
#define SC_TAB 0x0Fu
#define SC_CTRL 0x1Du
#define SC_LSHIFT 0x2Au
#define SC_RSHIFT 0x36u
#define SC_G 0x22u
#define SC_B 0x30u
#define SC_K 0x25u
#define X68_KEY_GROUPS 16u
static uint8_t s_x68_keys[X68_KEY_GROUPS]; /* held, by the X68000's key codes */
static uint32_t s_paused_frame = UINT32_MAX; /* the last frame the game waited in its pause */
#define KAKUSI_KEY_US 100000u               /* how long a K press stays new */
static uint32_t s_kakusi_us;                  /* when K was last pressed */
static bool s_kakusi_key;                     /* and not yet seen by the title */

/* The X68000's key code for an ST scancode, or 0: none the game reads. */
static uint32_t x68_key(uint32_t scancode) {
  switch (scancode) {
    case SC_ESC: return 0x01u;
    case SC_1: return 0x02u;
    case SC_TAB: return 0x10u;
    case SC_LSHIFT: case SC_RSHIFT: return 0x70u;
    case SC_CTRL: return 0x71u;
    default: return 0;
  }
}

void xl_paused(void) { s_paused_frame = s_frame_count; }

/* K pressed in the last moment (the title asks every frame): a press made
 * during a game is old by the next title. */
int xl_kakusi_key(void) {
  bool now = s_kakusi_key && time_us_32() - s_kakusi_us < KAKUSI_KEY_US;
  s_kakusi_key = false;
  return now;
}

/* The ST to GEM (the cartridge's command word: userfw leaves at its next
 * frame). */
static void exit_to_gem(void) {
  volatile uint32_t *sentinel =
      (volatile uint32_t *)((uintptr_t)__rom_in_ram_start__ + CART_CMD_SENTINEL_OFFSET);
  *sentinel = cart_asM68kLong(CART_CMD_BOOT_GEM);
}

/* The title's GEM DESKTOP and BOOSTER, as the pause's G and B. */
void xl_leave(uint32_t to) {
  if (to == 0) {
    exit_to_gem();
  } else {
    st_session_return_to_booster();
  }
}
static int s_live;
static uint8_t s_keys;  /* the stick's bits the keyboard holds */
static uint8_t s_stick;

int xl_live_input(void) { return 0; }
void xl_live_read(int on) { (void)on; }
int xl_record(void) { return 0; }

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
/* A write into a page the scene keeps in flash (xl.h's accessors): the game
 * stops with the address it wrote. */
void xl_stray_write(uint32_t address) { xl_fatal("a write into a page in flash", address); }
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

static const xl_store_entry_t *in_scene(uint32_t address);

uint32_t xl_call(uint32_t address) {
  address &= 0xFFFFFFu;
  const xl_store_entry_t *e = find(s_common, address);
  if (e == NULL) e = in_scene(address);
  return e->fn();
}

uint32_t xl_call_stage(uint32_t address) { return in_scene(address)->fn(); }

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
    (const void *)xl_live_read,     (const void *)xl_record,
    (const void *)&xl,              (const void *)xl_regions,
    (const void *)&xl_mem,          (const void *)xl_stray_write,
    (const void *)xl_paused,       (const void *)xl_kakusi_key,
    (const void *)xl_leave,
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

/* The host's checkpoints (XL_STORE_CHECKS_FILE on the card), read a pool
 * slot's worth at a time into the bench's slot (xl_store.h). */
#define CHECKS_PER_SLOT ((1u << XL_PAGE_BITS) / CHECKPOINT_BYTES)
#define BENCH_SLOT (POOL_PAGES - 1u - XL_STORE_OWNER_SLOTS - XL_STORE_BENCH_SLOTS)
static uint32_t s_check_count;  /* in the file */
static uint32_t s_check_first;  /* the first of the ones in the slot, or UINT32_MAX */
static uint32_t s_last_frame;   /* where the run ends (the start file's) */
static int s_invincible;        /* the start file's XL_STORE_START_INVINCIBLE */

static uint8_t *pool_page(uint32_t k);
static void card_path(char *out, const char *name);

static void open_checks(void) {
  char path[PATH_BYTES];
  card_path(path, XL_STORE_CHECKS_FILE);
  xl_store_checks_t h;
  s_check_count = 0;
  s_check_first = UINT32_MAX;
  if (store_read_open(path) != 0) return;
  if (store_read(&h, sizeof h) == (int)sizeof h && h.magic == XL_STORE_CHECKS_MAGIC &&
      h.version == XL_STORE_VERSION &&
      strncmp(h.firmware, release_build_id, XL_STORE_FIRMWARE_ID - 1u) == 0) {
    s_check_count = h.count;
  }
  store_read_close();
}

/* The host's checkpoint k, or NULL past its last. */
static const uint8_t *host_checkpoint(uint32_t k) {
  if (k >= s_check_count) return NULL;
  uint8_t *slot = pool_page(BENCH_SLOT);
  uint32_t first = k - k % CHECKS_PER_SLOT;
  if (first != s_check_first) {
    char path[PATH_BYTES];
    card_path(path, XL_STORE_CHECKS_FILE);
    uint32_t n = s_check_count - first < CHECKS_PER_SLOT ? s_check_count - first : CHECKS_PER_SLOT;
    s_check_first = UINT32_MAX;
    if (store_read_open(path) != 0) return NULL;
    if (store_read_seek(sizeof(xl_store_checks_t) + first * CHECKPOINT_BYTES) == 0 &&
        store_read(slot, n * CHECKPOINT_BYTES) == (int)(n * CHECKPOINT_BYTES)) {
      s_check_first = first;
    }
    store_read_close();
    if (s_check_first == UINT32_MAX) return NULL;
  }
  return slot + (k - first) * CHECKPOINT_BYTES;
}

/* As the host's XL_INVINCIBLE: the game's invincibility kept on, for a
 * recording of a game played so. */
static void keep_invincible(void) {
  if (s_invincible && rd16(GAME(INVINCIBILITY_MODE)) == 0) wr16(GAME(INVINCIBILITY_MODE), 1);
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
  const uint8_t *expected = host_checkpoint(k);
  if (expected == NULL) return;
  benchLogicResult.checkpoints++;
  if (memcmp(c, expected, CHECKPOINT_BYTES) != 0) {
    if (benchLogicResult.mismatches++ == 0) {
      benchLogicResult.first_mismatch = s_frame_count;
      memcpy(benchLogicMismatch, c, CHECKPOINT_BYTES);
      memcpy(benchLogicMismatch + CHECKPOINT_BYTES, expected, CHECKPOINT_BYTES);
    }
  }
}

/* A band of the frame (fb_draw_banded, on either core). */
/* Page 1's lines for the next band, streamed from flash (the XIP stream,
 * past the cache, at the flash's full rate) by DMA while this band draws,
 * into the next band's second half: free until the next band, which widens
 * them in place (xl_render_band_page1). One stream for both cores: a band
 * whose stream finds it busy, a half's first band, and lines that wrap past
 * page 1's end read the flash as they draw. Each core waits for its stream
 * in its next band, and a half's last band starts none: no stream outlives
 * a frame (a scene change writes the flash). */
#define STREAM_WORDS (FB_BAND_LINES * XL_RENDER_W / 2u / 4u)
#define STREAM_TIMEOUT_US 2000u
static int s_stream_dma[2] = {-1, -1};
static spin_lock_t *s_stream_lock;
static int s_stream_owner = -1;  /* the core whose stream runs, under the lock */
static int s_stream_band[2];     /* y0 + 1 of the band its lines are coming for */

static void stream_init(void) {
  if (s_stream_lock) return;
  s_stream_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
  s_stream_dma[0] = dma_claim_unused_channel(false);
  s_stream_dma[1] = dma_claim_unused_channel(false);
}

static bool stream_start(uint core, uint8_t *dst, const uint8_t *src) {
  int ch = s_stream_dma[core];
  if (ch < 0 || ((uintptr_t)src >> 24) != (XIP_BASE >> 24) || ((uintptr_t)src & 3u)) return false;
  /* Claimed and started under the lock: the other core sees a running DMA
   * or none, never a claim whose DMA has not started yet. The other core's
   * stream is waited for (it ends within ~50 us; drawing the lines from the
   * flash takes longer), up to STREAM_TIMEOUT_US. */
  uint32_t start = time_us_32(), save;
  for (;;) {
    save = spin_lock_blocking(s_stream_lock);
    if (s_stream_owner < 0 || !dma_channel_is_busy((uint)s_stream_dma[s_stream_owner])) break;
    spin_unlock(s_stream_lock, save);
    if (time_us_32() - start > STREAM_TIMEOUT_US) return false;
  }
  s_stream_owner = (int)core;
  while (!(xip_ctrl_hw->stat & XIP_STAT_FIFO_EMPTY)) (void)xip_ctrl_hw->stream_fifo;
  xip_ctrl_hw->stream_addr = (uint32_t)(uintptr_t)src;
  xip_ctrl_hw->stream_ctr = STREAM_WORDS;
  dma_channel_config cfg = dma_channel_get_default_config((uint)ch);
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
  channel_config_set_read_increment(&cfg, false);
  channel_config_set_write_increment(&cfg, true);
  channel_config_set_dreq(&cfg, DREQ_XIP_STREAM);
  dma_channel_configure((uint)ch, &cfg, dst, (const void *)XIP_AUX_BASE, STREAM_WORDS, true);
  spin_unlock(s_stream_lock, save);
  return true;
}

/* Whether the core's stream arrived: one takes ~50 us; one not done in
 * STREAM_TIMEOUT_US is stopped and the band reads the flash itself. */
static bool stream_wait(uint core) {
  uint ch = (uint)s_stream_dma[core];
  uint32_t start = time_us_32();
  bool done = true;
  while (dma_channel_is_busy(ch)) {
    if (time_us_32() - start > STREAM_TIMEOUT_US) {
      done = false;
      break;
    }
  }
  uint32_t save = spin_lock_blocking(s_stream_lock);
  if (!done) {
    dma_channel_abort(ch);
    xip_ctrl_hw->stream_ctr = 0;
    while (!(xip_ctrl_hw->stat & XIP_STAT_FIFO_EMPTY)) (void)xip_ctrl_hw->stream_fifo;
    benchPlayStats.stream_timeouts++;
  }
  if (s_stream_owner == (int)core) s_stream_owner = -1;
  spin_unlock(s_stream_lock, save);
  return done;
}

static void draw_band(uint8_t *band, const void *arg, int y0, int y1) {
  const xl_render_frame_t *f = (const xl_render_frame_t *)arg;
  uint core = get_core_num();
  const uint8_t *lines = NULL;
  if (s_stream_band[core] == y0 + 1 && stream_wait(core)) {
    lines = band + FB_BAND_BYTES / 2u;
    benchPlayStats.streamed_bands++;
  } else {
    benchPlayStats.unstreamed_bands++;
  }
  s_stream_band[core] = 0;
  if (y1 % (FB_CHUNKED_H / 2) != 0) {
    uint32_t first = (f->page1_y + (uint32_t)y1) % f->art->page_lines;
    if (first + FB_BAND_LINES <= f->art->page_lines &&
        stream_start(core, band + FB_BAND_BYTES, f->art->page1 + first * (XL_RENDER_W / 2u))) {
      s_stream_band[core] = y1 + 1;
    }
  }
  xl_render_band_page1(band, f, y0, y1, lines);
}

static void note(uint32_t *sum, uint32_t *max, uint32_t us) {
  *sum += us;
  if (us > *max) *max = us;
}

/* The held frame drawn again with each part left out (benchHoldTimes). */
static void hold_times(void) {
  static const uint32_t skips[4] = {0, XL_RENDER_SKIP_SPRITES, XL_RENDER_SKIP_TEXT,
                                    XL_RENDER_SKIP_PAGES};
  bench_hold_times_t t = {benchPlayHoldAt, s_frame.sprite_count, {0}, {0}};
  for (uint32_t k = 0; k < 4; k++) {
    s_frame.skip = skips[k];
    xip_ctrl_hw->ctr_hit = 0; /* a write clears either counter */
    xip_ctrl_hw->ctr_acc = 0;
    uint32_t t0 = time_us_32();
    for (uint32_t n = 0; n < BENCH_HOLD_DRAWS; n++) fb_draw_banded(draw_band, &s_frame);
    t.us[k] = (time_us_32() - t0) / BENCH_HOLD_DRAWS;
    t.misses[k] = (xip_ctrl_hw->ctr_acc - xip_ctrl_hw->ctr_hit) / BENCH_HOLD_DRAWS;
  }
  s_frame.skip = 0;
  benchHoldTimes = t;
}

/* The player's keys and stick, from the ST's IKBD through the ROM3 ring:
 * every frame, and from the game's own reads of them (_BITSNS, _JOYGET),
 * which loop without a frame while they wait for a key to be let go or
 * pressed (the pause). */
static void poll_input(void) {
  fb_pump_rom3();
  ikbd_pump();
  /* The ST started again (a reset, a return from GEM): the game starts over
   * at its title, as the template's apps do, not on in a pause the new
   * screen would not show. */
  if (st_session_consume_boot()) stop(BENCH_LOGIC_DONE, "", 0);
  ikbd_key_event_t key;
  while (ikbd_pop_key(&key)) {
    uint8_t bit = key.scancode == SC_UP      ? STICK_UP
                  : key.scancode == SC_DOWN  ? STICK_DOWN
                  : key.scancode == SC_LEFT  ? STICK_LEFT
                  : key.scancode == SC_RIGHT ? STICK_RIGHT
                  : key.scancode == SC_SPACE ? STICK_BOMB
                  : key.scancode == SC_ALT   ? STICK_SHOT
                                             : 0u;
    s_keys = key.is_press ? (uint8_t)(s_keys | bit) : (uint8_t)(s_keys & ~bit);
    uint32_t code = x68_key(key.scancode);
    if (code) {
      uint8_t mask = (uint8_t)(1u << (code & 7u));
      s_x68_keys[code >> 3] = key.is_press ? (uint8_t)(s_x68_keys[code >> 3] | mask)
                                           : (uint8_t)(s_x68_keys[code >> 3] & ~mask);
      if (key.is_press) bit = 1; /* a player's key: the game is the player's */
    }
    if (key.is_press && s_frame_count - s_paused_frame <= 2u) {
      if (key.scancode == SC_G) exit_to_gem();
      if (key.scancode == SC_B) st_session_return_to_booster();
    }
    if (key.is_press && key.scancode == SC_K) {
      s_kakusi_key = true;
      s_kakusi_us = time_us_32();
    }
    if (key.is_press && bit && !s_live) {
      s_live = 1;
      input_close();
    }
  }
  ikbd_joystick_t stick;
  ikbd_read_joystick(1, &stick);
  s_stick = (uint8_t)(s_keys | ((stick.state & IKBD_JOY_UP) ? STICK_UP : 0u) |
                      ((stick.state & IKBD_JOY_DOWN) ? STICK_DOWN : 0u) |
                      ((stick.state & IKBD_JOY_LEFT) ? STICK_LEFT : 0u) |
                      ((stick.state & IKBD_JOY_RIGHT) ? STICK_RIGHT : 0u) |
                      ((stick.state & IKBD_JOY_FIRE) ? STICK_SHOT : 0u));
  if (s_stick) {
    s_live = 1;
    input_close(); /* the player takes over from a script */
  }
}

/* Play mode: the frame the game has just finished, drawn and converted in
 * bands on both cores (the chunky buffer's other part holds game pages) and
 * shown (fb_republish waits for the ST to have copied the last one: one
 * game frame a VBL), with what the main loop would do meanwhile. */
static void present(uint32_t game_us) {
  uint32_t t0 = time_us_32();
  s_frame.art = s_stage->art;
  xl_render_prepare(&s_frame);
  fb_draw_banded(draw_band, &s_frame);
  if (xl_render_palette(s_stage->art, s_palette)) palette_set(s_palette);
  uint32_t t1 = time_us_32();
  fb_republish();
  uint32_t t2 = time_us_32();
  static uint32_t s_shown_us;
  uint32_t gap = t2 - s_shown_us;
  if (s_shown_us && gap > BENCH_LATE_US && gap < 1000000u &&
      s_stage->kind < BENCH_SCENE_KINDS) {
    benchPlayStats.late[s_stage->kind]++;
  }
  s_shown_us = t2;
  audio_render_frame(); /* the music and the effects: song_samples */
  uint32_t t3 = time_us_32();
  poll_input();
  select_poll();
  if (benchPlayHoldAt && s_frame_count == benchPlayHoldAt + 1u) {
    hold_times();
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
  keep_invincible();
  if (s_frame_count >= s_last_frame) stop(BENCH_LOGIC_DONE, "", 0);
  s_frame_end_us = time_us_32();
}

void xl_patch_wait_vbl(void) {
  uint32_t ticks = rd16(A(7) + 6);
  D(1) = (D(1) & 0xFFFF0000u) | ticks;
  s_last_sprite_count = s_frame_sprite_count;
  s_frame_sprite_count = 0;
  s_frame_count++;
  /* The menus' frames: the picture before the wait's VBLs, as the host
   * takes it (xl_host.c's WAIT_VBL). */
  if (s_playing) present(time_us_32() - s_frame_end_us);
  if (!(ticks & 0x8000u)) {
    for (uint32_t k = 0; k <= ticks; k++) run_vbl();
  }
  if (s_frame_count % CHECKPOINT_INTERVAL == 0) checkpoint();
  keep_invincible();
  if (s_frame_count >= s_last_frame) stop(BENCH_LOGIC_DONE, "", 0);
  s_frame_end_us = time_us_32();
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
    case 0x04: /* _BITSNS: d1 the group, the keys of it held */
      if (s_playing) poll_input();
      D(0) = s_x68_keys[D(1) & (X68_KEY_GROUPS - 1u)];
      return;
    case 0x3B: /* _JOYGET: the script's (stick 1 only, as the host's), or the player's stick */
      if (s_input.on) {
        D(0) = (D(1) & 0xFFu) == 0 ? input_joyget() : 0xFFFFFFFFu;
      } else {
        if (s_playing) poll_input();
        D(0) = 0xFFFFFF00u | (~(uint32_t)s_stick & 0xFFu);
      }
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
    /* The rest as the host serves them (tools/xlat/host/xl_host.c iocs): the
     * title's, the menus' and the continue screen's. */
    case 0x92: { /* _VC_R1: d1.w the new R1, -1 reads it; returns the previous */
      uint32_t prev = rd16(0xE82500u);
      D(0) = prev;
      if ((D(1) & 0xFFFFu) != 0xFFFFu) wr16(0xE82500u, D(1));
      return;
    }
    case 0x14: { /* _TPALET2: d1 entry, d2 colour (negative: read) */
      uint32_t at = 0xE82200u + (D(1) & 0xFu) * 2u;
      D(0) = 0;
      if ((int32_t)D(2) < 0) {
        D(0) = rd16(at);
      } else {
        wr16(at, D(2));
      }
      return;
    }
    case 0x7F: /* _ONTIME: fixed, as the host's */
      D(0) = 100u * 100u;
      D(1) = 0;
      return;
    case 0xCF: { /* _SPALET: d1 entry, d2 block, d3 colour (negative: read) */
      uint32_t index = (((D(2) & 0xFFFFu) << 4) + (D(1) & 0xFFFFu)) & 0xFFFFu;
      uint32_t at = 0xE82200u + (uint32_t)(int32_t)(int16_t)index * 2u;
      D(0) = 0;
      if ((int32_t)D(3) < 0) {
        D(0) = rd16(at);
      } else {
        wr16(at, D(3));
      }
      return;
    }
    case 0xCE: { /* _BGTEXTGT: d1 page, d2 x, d3 y: the BG map's word */
      uint32_t at = 0xEB8000u + 0x4000u + (D(1) & 1u) * 0x2000u +
                    ((((D(3) & 63u) << 6) + (D(2) & 63u)) & 0xFFFFu) * 2u;
      D(0) = rd16(at);
      return;
    }
    /* The calls the host answers with 0 too (tools/xlat/host/xl_host.c):
     * nothing the RP's picture or sound depends on. */
    case 0x10: case 0x20: case 0x22: case 0x23: case 0x7D: case 0x87: case 0x90:
    case 0xAE: case 0xAF: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
    case 0xC1: case 0xC2: case 0xCA:
      D(0) = 0;
      return;
    default: {
      static char what[48];
      snprintf(what, sizeof what, "IOCS $%02X, which the bench does not serve",
               (unsigned)(D(0) & 0xFFu));
      xl_fatal(what, pc);
    }
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
  benchPlayStats.effects++;
  if (!s_fx.open || address < s_fx.base || address - s_fx.base >= s_fx.size) {
    benchPlayStats.effects_missed++;
    return;
  }
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
  if (benchPlayStats.songs < BENCH_SONG_LOG) {
    benchPlayStats.song_log[benchPlayStats.songs] =
        s_frame_count << 8 | (index & 0x7Fu) | BENCH_SONG_MISSED;
  }
  benchPlayStats.songs++;
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
  if (benchPlayStats.songs <= BENCH_SONG_LOG) {
    benchPlayStats.song_log[benchPlayStats.songs - 1u] &= ~BENCH_SONG_MISSED;
  }
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
      benchPlayStats.fades++;
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

/* _NAMECK: a file's name taken apart into `buffer` (91 bytes: the drive,
 * the path from 2, the name from 67, the extension from 86), as the host's
 * runtime does it (tools/xlat/host/xl_host.c nameck). */
static void nameck(uint32_t name, uint32_t buffer) {
  for (uint32_t k = 0; k < 91; k++) wr8(buffer + k, 0);
  uint32_t s = name;
  wr8(buffer, 'A');
  wr8(buffer + 1, ':');
  if (rd8(s) && rd8(s + 1) == ':') {
    wr8(buffer, rd8(s));
    s += 2;
  }
  uint32_t file = s;
  for (uint32_t p = s; rd8(p); p++) {
    if (rd8(p) == '\\' || rd8(p) == '/') file = p + 1;
  }
  uint32_t out = buffer + 2;
  wr8(out++, '\\');
  for (uint32_t p = s, n = 0; p < file && n < 63; p++, n++) {
    uint32_t ch = rd8(p) == '/' ? '\\' : rd8(p);
    if (ch == '\\' && rd8(out - 1) == '\\') continue;
    wr8(out++, ch);
  }
  if (rd8(out - 1) != '\\') wr8(out++, '\\');
  out = buffer + 67;
  uint32_t p = file;
  for (int n = 0; n < 18 && rd8(p) && rd8(p) != '.'; n++) wr8(out++, rd8(p++));
  while (rd8(p) && rd8(p) != '.') p++;
  if (rd8(p) == '.') {
    out = buffer + 86;
    for (int n = 0; n < 4 && rd8(p); n++) wr8(out++, rd8(p++));
  }
}

/* The game's files (its saves) in the app's folder on the card, by their
 * names without the X68000's directory (ETC_DAT\SCORE.DAT is SCORE.DAT). One
 * open at a time, as the game opens them, under the handle DOS_FILE.
 * DEMO.REP's writes are dropped (DOS_DROPPED): the RP records no game, so the
 * save would write the loaded demo back. */
#define DOS_FILE 5u
#define DOS_DROPPED 6u
#define DOS_CHUNK 64u
#define DOS_NAME 16u
static FIL s_dos_file; /* static: a FIL does not belong on core 0's stack */
static int s_dos_open;

/* The base name of the 68000 string at `name`, upper case. */
static void dos_name(char *out, uint32_t name) {
  uint32_t from = name, n = 0;
  for (uint32_t p = name; rd8(p); p++) {
    uint32_t c = rd8(p);
    if (c == '\\' || c == '/' || c == ':') from = p + 1;
  }
  for (uint32_t p = from; rd8(p) && n < DOS_NAME - 1u; p++) {
    uint32_t c = rd8(p);
    out[n++] = (char)(c >= 'a' && c <= 'z' ? c - 32u : c);
  }
  out[n] = 0;
}

static uint32_t dos_open(uint32_t name, BYTE mode) {
  char base[DOS_NAME], path[PATH_BYTES];
  dos_name(base, name);
  /* A scripted game is a test: the card's files stay the player's. */
  if (strcmp(base, "DEMO.REP") == 0 || s_scripted) return DOS_DROPPED;
  if (s_dos_open) return (uint32_t)-4; /* too many files open */
  card_path(path, base);
  if (f_open(&s_dos_file, path, mode) != FR_OK) return (uint32_t)-2;
  s_dos_open = 1;
  return DOS_FILE;
}

static void dos_close(void) {
  if (s_dos_open) f_close(&s_dos_file);
  s_dos_open = 0;
}

/* _READ (`write` 0) or _WRITE: `length` bytes between the file and the game's
 * memory at `buffer`, DOS_CHUNK at a time; a write of none truncates the file
 * there, as on the X68000. */
static uint32_t dos_transfer(int write, uint32_t buffer, uint32_t length) {
  uint8_t chunk[DOS_CHUNK];
  if (write && length == 0) return f_truncate(&s_dos_file) == FR_OK ? 0 : (uint32_t)-1;
  uint32_t done = 0;
  while (done < length) {
    UINT want = length - done < DOS_CHUNK ? (UINT)(length - done) : DOS_CHUNK, got = 0;
    if (write) {
      for (UINT k = 0; k < want; k++) chunk[k] = (uint8_t)rd8(buffer + done + k);
      if (f_write(&s_dos_file, chunk, want, &got) != FR_OK) return (uint32_t)-1;
    } else {
      if (f_read(&s_dos_file, chunk, want, &got) != FR_OK) return (uint32_t)-1;
      for (UINT k = 0; k < got; k++) wr8(buffer + done + k, chunk[k]);
    }
    done += got;
    if (got < want) break;
  }
  return done;
}

/* The high scores the card keeps (SCORE.DAT: the table as it is in memory),
 * over the image's, and the top score from them, as the game's boot reads
 * them; without the file the image's stay. */
#define SCORE_BYTES 240u /* ten entries of 24 bytes */
static void load_scores(void) {
  char path[PATH_BYTES];
  uint8_t table[SCORE_BYTES];
  UINT got = 0;
  dos_close();
  card_path(path, "SCORE.DAT");
  if (f_open(&s_dos_file, path, FA_READ) != FR_OK) return;
  if (f_read(&s_dos_file, table, SCORE_BYTES, &got) == FR_OK && got == SCORE_BYTES) {
    for (uint32_t k = 0; k < SCORE_BYTES; k++) wr8(GAME(WORD_0008D994) + k, table[k]);
    wr32(GAME(GAME_HIGHSCORE), rd32(GAME(WORD_0008D994)));
  }
  f_close(&s_dos_file);
}

/* The DOS calls the game makes after its boot (tools/xlat/host/xl_host.c's
 * "xl: dos" log): the ones the host answers with 0, _SUPER among them (a
 * palette register written in supervisor mode on the continue and game-over
 * screens), and the file calls of its saves (the high scores at a game over
 * and from CONFIG, the replay at a new top score) on the card. The FLOAT
 * library's multiply and divide as the host serves them. Anything else stops
 * the run. The flags as a move of the result, as the port returns them. */
void xl_linef(uint32_t word, uint32_t pc) {
  uint32_t args = A(7), r;
  uint32_t handle = rd16(args);
  switch (word) {
    case 0xFE00: /* __LMUL (the C library's file writes) */
      r = (uint32_t)((int32_t)D(0) * (int32_t)D(1));
      break;
    case 0xFE01: /* __LDIV */
      if (D(1) == 0) xl_divide_by_zero(pc);
      r = (uint32_t)((int32_t)D(0) / (int32_t)D(1));
      break;
    case 0xFE0D: case 0xFF0D: case 0xFF20: case 0xFF21: case 0xFF23: case 0xFF25: case 0xFF44:
      r = 0;
      break;
    case 0xFF36: /* _DSKFRE */
      r = 10000000u;
      break;
    case 0xFF37: /* _NAMECK */
      nameck(rd32(args), rd32(args + 4u));
      r = 0;
      break;
    case 0xFF3C: /* _CREATE */
      r = dos_open(rd32(args), FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
      break;
    case 0xFF3D: { /* _OPEN: an existing file; mode 0 read, 1 write, 2 both */
      uint32_t mode = rd16(args + 4u) & 3u;
      r = dos_open(rd32(args), mode == 0 ? FA_READ : mode == 1 ? FA_WRITE : FA_READ | FA_WRITE);
      break;
    }
    case 0xFF3E: /* _CLOSE */
      if (handle == DOS_FILE && s_dos_open) {
        dos_close();
        r = 0;
      } else {
        r = handle == DOS_DROPPED ? 0u : (uint32_t)-6;
      }
      break;
    case 0xFF3F: case 0xFF40: { /* _READ, _WRITE */
      uint32_t buffer = rd32(args + 2u), length = rd32(args + 6u) & 0x7FFFFFFFu;
      if (handle == DOS_DROPPED) {
        r = word == 0xFF40 ? length : 0u;
      } else if (handle == DOS_FILE && s_dos_open) {
        r = dos_transfer(word == 0xFF40, buffer, length);
      } else {
        r = (uint32_t)-6;
      }
      break;
    }
    case 0xFF41: { /* _DELETE */
      char base[DOS_NAME], path[PATH_BYTES];
      dos_name(base, rd32(args));
      card_path(path, base);
      r = strcmp(base, "DEMO.REP") == 0 || f_unlink(path) == FR_OK ? 0u : (uint32_t)-2;
      break;
    }
    case 0xFF42: /* _SEEK: from the start, the position, the end; the new position */
      if (handle == DOS_FILE && s_dos_open) {
        uint32_t whence = rd16(args + 6u) & 3u;
        FSIZE_t base = whence == 0 ? 0 : whence == 1 ? f_tell(&s_dos_file) : f_size(&s_dos_file);
        FSIZE_t to = base + (FSIZE_t)(int32_t)rd32(args + 2u);
        r = f_lseek(&s_dos_file, to) == FR_OK ? (uint32_t)f_tell(&s_dos_file) : (uint32_t)-1;
      } else {
        r = handle == DOS_DROPPED ? 0u : (uint32_t)-6;
      }
      break;
    default:
      xl_fatal("a DOS or FLOAT call", pc);
      return;
  }
  D(0) = r;
  xl.n = (r >> 31) & 1u;
  xl.z = r == 0;
  xl.v = 0;
  xl.c = 0;
}

/* --- The run --------------------------------------------------------------- */

/* The app's folder on the card, and a file in it. */
static void card_path(char *out, const char *name) {
  SettingsConfigEntry *folder = settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  snprintf(out, PATH_BYTES, "%s/%s", folder ? folder->value : ACONFIG_DEFAULT_FOLDER, name);
}

/* Between flash operations: the bus, the IKBD and SELECT keep going. */
static void pump(void) {
  fb_pump_rom3();
  ikbd_pump();
  select_poll();
}

/* The install gate's: the same, and the sound path the ST chose at its
 * hello (audio.c follows it only here, before the game), which decides how
 * a frame is laid out for the ST's copy (the blitter's order or the CPU's). */
static void gate_pump(void) {
  pump();
  audio_render_frame();
}

static const xl_store_header_t *load(int k, const char *name, uint32_t base, uint32_t size) {
  char path[PATH_BYTES];
  card_path(path, name);
  uint32_t t0 = time_us_32();
  /* Through the chunky buffer's banded part: no frame is drawn during a load.
   * Tried again when the card failed it: just after a power-on both images
   * once read as no image, and loaded on the next try. */
  int status = store_load(path, base, size, release_build_id, fb_chunked_buffer, pump);
  for (int again = 0; again < 2 && (status == STORE_NO_FILE || status == STORE_BAD_FILE ||
                                    status == STORE_READ_ERROR); again++) {
    sleep_ms(100);
    status = store_load(path, base, size, release_build_id, fb_chunked_buffer, pump);
  }
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
  if (k < POOL_FB_PAGES) return fb_chunked_buffer + XL_STORE_FB_FREE + (k << XL_PAGE_BITS);
  k -= POOL_FB_PAGES;
  if (k < POOL_RAM_PAGES) return xl_rp_ram + (k << XL_PAGE_BITS);
  return (uint8_t *)__rom_in_ram_start__ + XL_STORE_WINDOW_FREE +
         ((k - POOL_RAM_PAGES) << XL_PAGE_BITS);
}

/* Which page each pool slot holds (16 bits a slot, in the slots below the
 * spare: xl_store.h's XL_STORE_OWNER_SLOTS), kept across scene changes. */
#define OWNERS_PER_SLOT ((1u << XL_PAGE_BITS) / 2u)
#define OWNER_SLOT (POOL_PAGES - 1u - XL_STORE_OWNER_SLOTS)
_Static_assert(XL_STORE_OWNER_SLOTS * OWNERS_PER_SLOT >= POOL_PAGES, "an owner a slot");

static uint16_t *slot_owner(uint32_t slot) {
  return (uint16_t *)pool_page(OWNER_SLOT + slot / OWNERS_PER_SLOT) + slot % OWNERS_PER_SLOT;
}

/* The scene's pages are where they are now. */
static void owners_of(const xl_store_header_t *h) {
  for (uint32_t k = 0; k < h->ram_count; k++) *slot_owner(h->ram_list[k].slot) = h->ram_list[k].page;
}

/* The scene holding `address` (COMMON's table), or 0. */
static uint32_t scene_of(uint32_t address) {
  uint32_t lo = 0, hi = s_common->scene_count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2u;
    if (s_common->scenes[mid].address < address) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return lo < s_common->scene_count && s_common->scenes[lo].address == address
             ? s_common->scenes[lo].kind
             : 0u;
}

static void scene_file(char *name, uint32_t kind) {
  if (kind == XL_STORE_KIND_MENUS) {
    strcpy(name, "MENUS.IMG");
  } else {
    snprintf(name, 16, "STAGE%lu.IMG", (unsigned long)kind);
  }
}

/* The scene change: the slot copied from the scene's image on the card, its
 * page table installed, and its pages whose slot another scene's page has
 * held since set to their content after the boot (each page keeps one slot
 * in every scene: the rest stay as they are). It runs inside a call from
 * COMMON's code (the stages' routines are called from COMMON's frame loop,
 * the menus from its top loop), so no code of the slot is running. The ST
 * shows the last frame meanwhile. */
static void scene_change(uint32_t kind) {
  uint32_t t0 = time_us_32();
  audio_pause(true); /* the copy blocks the main loop, its erases the interrupts */
  char name[16];
  scene_file(name, kind);
  const xl_store_header_t *h = load(1, name, XL_STORE_SLOT, XL_STORE_SLOT_SIZE);
  if (h == NULL || h->kind != kind || h->ram_count >= POOL_PAGES) xl_fatal("no image for the scene", kind);
  s_stage = h;
  for (int r = 0; r < 256; r++) xl_regions[r] = (uint8_t **)h->regions[r];
  /* The scene's colours with its own art (it sits where the last one did). */
  xl_render_palette_forget();
  uint32_t set = 0;
  for (uint32_t k = 0; k < h->entry_pages; k++) {
    const xl_store_ram_page_t *e = &h->ram_list[k];
    if (*slot_owner(e->slot) != e->page) {
      memcpy(pool_page(e->slot), h->entry_init + (k << XL_PAGE_BITS), 1u << XL_PAGE_BITS);
      set++;
    }
  }
  owners_of(h);
  audio_pause(false);
  benchLogicResult.scene_changes++;
  benchLogicResult.scene_kind = kind;
  benchLogicResult.scene_frame = s_frame_count;
  benchLogicResult.scene_us = time_us_32() - t0;
  benchLogicResult.scene_pages = set;
  s_frame_end_us = time_us_32(); /* the copy is not the game's time */
}

/* `address`'s function in the slot's scene, changing scene first when
 * another holds it. */
static const xl_store_entry_t *in_scene(uint32_t address) {
  const xl_store_entry_t *e = find(s_stage, address);
  if (e == NULL) {
    uint32_t kind = scene_of(address);
    if (kind == 0 || kind == s_stage->kind) xl_fatal("no translated code", address);
    benchLogicResult.scene_address = address;
    scene_change(kind);
    e = find(s_stage, address);
    if (e == NULL) xl_fatal("not in its scene's image", address);
  }
  return e;
}

/* The start's memory from the card (XL_STORE_START_FILE): the start scene's
 * RAM pages into their slots, the text plane's rows. NULL, or what is wrong. */
static const char *read_start(void) {
  char path[PATH_BYTES];
  card_path(path, XL_STORE_START_FILE);
  if (store_read_open(path) != 0) return "no start file on the card";
  xl_store_start_t h;
  const char *problem = NULL;
  if (store_read(&h, sizeof h) != (int)sizeof h || h.magic != XL_STORE_START_MAGIC ||
      h.version != XL_STORE_VERSION ||
      strncmp(h.firmware, release_build_id, XL_STORE_FIRMWARE_ID - 1u) != 0 ||
      h.kind != s_stage->kind || h.ram_count != s_stage->ram_count) {
    problem = "the start file is not these images'";
  }
  s_last_frame = h.last_frame;
  s_invincible = (h.flags & XL_STORE_START_INVINCIBLE) != 0;
  s_scripted = (h.flags & XL_STORE_START_SCRIPTED) != 0;
  for (uint32_t k = 0; !problem && k < h.ram_count; k++) {
    void *page = pool_page(s_stage->ram_list[k].slot);
    if (store_read(page, 1u << XL_PAGE_BITS) != (int)(1u << XL_PAGE_BITS)) problem = "a short start file";
  }
  memset(xl_rp_text, 0, sizeof xl_rp_text);
  /* The rows' offsets through the chunky buffer's banded part (no frame is
   * drawn yet), then each row into place. */
  uint16_t *rows = (uint16_t *)fb_chunked_buffer;
  uint32_t offsets = (h.text_row_count * 2u + 3u) & ~3u;
  if (!problem && (offsets > FB_BANDED_BYTES || store_read(rows, offsets) != (int)offsets)) {
    problem = "a short start file";
  }
  for (uint32_t k = 0; !problem && k < h.text_row_count; k++) {
    if (rows[k] + 32u > sizeof xl_rp_text || store_read(xl_rp_text + rows[k], 32u) != 32) {
      problem = "a short start file";
    }
  }
  store_read_close();
  /* The slots hold the start scene's pages, the others nothing yet. */
  for (uint32_t k = 0; k < POOL_PAGES; k++) *slot_owner(k) = 0xFFFFu;
  owners_of(s_stage);
  return problem;
}

static void restore(void) {
  if (s_playing) {
    s_song.transferred = s_stage->song_playing;
    song_play(s_stage->song_playing, s_stage->song_frames);
  }
  s_live = 0;
  for (int r = 0; r < 256; r++) xl_regions[r] = (uint8_t **)s_stage->regions[r];
  memset(pool_page(POOL_PAGES - 1u), 0, 1u << XL_PAGE_BITS); /* the spare */
  xl_render_palette_forget();
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
  /* The list the game handed over last, still on the screen (the title sets
   * its logo's once): in its table, which the game leaves as it was. */
  if (s_playing) xl_render_take_sprites(&s_frame, GAME(SPRITE_DATA_TABLE), s_sprite_list_count);
}

bool bench_logic_available(void) { return true; }

/* Load the images and play from the snapshot to the start file's last frame: timed (the
 * logic bench), or drawn and shown, over and over until the host says
 * otherwise (play). */
static void run(int drawn);

void bench_logic_run(void) { run(0); }

static void run(int drawn) {
  (void)st_session_consume_boot(); /* the ST's start before this run's */
  memset(&benchLogicResult, 0, sizeof benchLogicResult);
  memset(benchLogicHistogram, 0, sizeof benchLogicHistogram);
  stream_init();
  /* Called from the main loop, which joins every core 1 job before it goes
   * on (fb_chunked.c): core 1 is parked in RAM, the flash may be written. */
  benchLogicResult.state = BENCH_LOGIC_LOADING;
  s_common = load(0, "COMMON.IMG", XL_STORE_COMMON, XL_STORE_COMMON_SIZE);
  s_stage = load(1, START_IMAGE, XL_STORE_SLOT, XL_STORE_SLOT_SIZE);
  storeForceCopy = 0;
  benchLogicResult.state = BENCH_LOGIC_IDLE;
  const char *problem = NULL;
  if (s_common == NULL || s_stage == NULL || s_stage->state == NULL) {
    problem = "no images for this firmware";
  } else if (s_stage->ram_count >= POOL_PAGES) {
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
    if (s_stage->kind != XL_STORE_KIND_MENUS) { /* a lap ended in another scene */
      s_stage = load(1, START_IMAGE, XL_STORE_SLOT, XL_STORE_SLOT_SIZE);
      if (s_stage == NULL || s_stage->state == NULL) {
        benchLogicResult.state = BENCH_LOGIC_ERROR;
        strncpy(benchLogicResult.error, "no images for this firmware",
                sizeof benchLogicResult.error - 1u);
        break;
      }
    }
    const char *wrong = read_start();
    open_checks();
    if (wrong) {
      benchLogicResult.state = BENCH_LOGIC_ERROR;
      strncpy(benchLogicResult.error, wrong, sizeof benchLogicResult.error - 1u);
      break;
    }
    restore();
    if (!s_scripted) load_scores(); /* a scripted game starts as the host's does */
    input_open();
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
  input_close(); /* the card's files free again (swd.py sd-put) */
  dos_close();
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
    gate_pump();
    /* An ST that started again shows nothing until a new frame (laid out
     * for the copy it chose: gate_pump follows its sound path). */
    if (st_session_consume_boot()) fb_republish();
    if (ikbd_pop_key(&key) && key.is_press) st_session_return_to_booster();
  }
}

/* The YM path's level curve (audio_set_ym_curve): 127 tanh(G x) / tanh(G)
 * for x = sample / 127, G = YM_LOUDNESS: about G times louder (+6 dB at 2)
 * for quiet samples, rounding off to the same peak. Made once. */
#define YM_LOUDNESS 2.0
static const uint8_t *ym_curve(void) {
  static uint8_t curve[256];
  static bool made;
  if (!made) {
    for (int i = 0; i < 256; i++) {
      double y = 127.0 * tanh(YM_LOUDNESS * (i - 128) / 127.0) / tanh(YM_LOUDNESS);
      int v = (int)lround(y);
      curve[i] = (uint8_t)((v < -128 ? -128 : v > 127 ? 127 : v) + 128);
    }
    made = true;
  }
  return curve;
}

/* Play until the host stops it or the run fails (then not again until the
 * host asks: benchPlay back to 1). */
static void play(void) {
  uint32_t hz = sdcard_getSpiHz();
  if (hz < PLAY_MIN_SD_HZ) {
    refuse_slow_card(hz);
    return;
  }
  /* The sound made from the repository's zip, once (install.c); the game's
   * RAM pages and the chunky buffer past its banded part (the gate draws in
   * bands) are free until the run starts. */
  SettingsConfigEntry *folder = settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  /* At the card's fastest (SDCARD_MAX_KHZ) whatever its setting, which the
   * gate's writes are bound by; the setting again for the game. */
  /* The songs are made for the sound path the machine plays through, which
   * it tells at its hello (an ST just switched on may not have said it yet:
   * the game needs the ST anyway). */
  while (st_session_hellos() == 0) gate_pump();
  gate_pump();
  int path = audio_uses_dma() ? INSTALL_FOR_DMA : INSTALL_FOR_YM;
  uint32_t card_hz = sdcard_setSpiHzNow(SDCARD_MAX_KHZ * SDCARD_KILOBAUD);
  int installed = install_gate(folder ? folder->value : ACONFIG_DEFAULT_FOLDER, xl_rp_ram,
                               fb_chunked_buffer + XL_STORE_FB_FREE, path, release_build_id, gate_pump);
  if (card_hz) sdcard_setSpiHzNow(card_hz);
  if (installed != INSTALL_READY) {
    benchLogicResult.state = BENCH_LOGIC_ERROR;
    snprintf(benchLogicResult.error, sizeof benchLogicResult.error, "install gate: %d", installed);
    install_refuse(installed, folder ? folder->value : ACONFIG_DEFAULT_FOLDER, gate_pump);
    return;
  }
  /* The lines the ST copies a frame: all 256 when the sound goes through
   * the DMA chip; the top 240 on the YM (a plain ST, a Mega ST), whose
   * samples the copy writes and which has no time for 256 at 50 fps. */
  fb_set_copy_lines(path == INSTALL_FOR_DMA ? CART_FB_LINES : CART_FB_LINES_SHORT);
  /* The YM plays quieter than the DMA chip: its quiet and middle levels
   * raised about 6 dB (ym_curve), its peaks kept. */
  audio_set_ym_curve(ym_curve());
  ikbd_set_input_mode(IKBD_INPUT_JOYSTICKS); /* stick 1 for the player */
  ikbd_set_esc_auto_exit(false);             /* ESC is the game's pause */
  run(1);
  ikbd_set_esc_auto_exit(true);
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
