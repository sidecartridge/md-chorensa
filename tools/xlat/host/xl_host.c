/* The translated program on the host: the X68000 environment it runs in, as
 * the Amiga port's emulator gives it to the original code (the oracle was
 * recorded there), and the frontend that plays the attract demo, records the
 * game's state every 60 frames (the oracle's checkpoints) and its screen.
 *
 * Built with the translator's output (tools/xlat/xlat.py), from the user's
 * copy of the program: never committed. tools/xlat/run_host.py builds and
 * runs it.
 *
 * usage: xl_host DATA_DIR FRAMES OUT_DIR [--screens N] [--snapshot F] [--resume FILE]
 *                [--entries 1]
 *   DATA_DIR  the game's data (the *_DAT folders) and sincos.dat; the program
 *             opens its files relative to it, and writes into it
 *   FRAMES    the frames to play (the oracle: 6,000)
 *   OUT_DIR   checkpoints.bin (the port's format: tools/re/oracle.py) and
 *             screen_NNNNN.bin every N frames (500 by default)
 *   --snapshot F  at the first XSP_VSYNC that ends at frame F or later:
 *             snapshot.bin (the runtime's state and the whole memory) and
 *             resume.txt (the entries a resumed run returns into, for
 *             xlat.py --resume)
 *   --resume FILE  start from a snapshot instead of the program's entry
 *   --entries 1  beside each screen, entries_NNNNN.bin: each pixel's palette
 *             entry and the palettes (write_entries)
 */
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xl.h"
#include "xl_functions.h"
#include "xl_layout.h"
#ifdef XL_RP_RENDER
#include "xl_render.h"
#endif

xl_cpu_t xl;
uint8_t *xl_mem;

typedef uint32_t (*xl_fn_t)(void);
extern const struct {
  uint32_t address;
  xl_fn_t fn;
} xl_entries[];
extern const uint32_t xl_entry_count;

/* The Human68k process area (memory block, stack, heap), as the port's. */
#define HEAP_ADDRESS 0x100000u
#define HEAP_SIZE 0x200000u
#define FIRST_FILE_HANDLE 5
#define FILE_HANDLES 16
#define CHECKPOINT_INTERVAL 60
#define MAXIMUM_SPRITES 512
#define FRAME_TIMER_HZ100 5546u /* 55.46 Hz */

static const char *s_data_dir;
static const char *s_out_dir;
static uint32_t s_frame_limit, s_screen_interval = 500;
static uint32_t s_frame_count, s_total_vbl, s_frame_sprite_count, s_last_sprite_count;
static FILE *s_files[FILE_HANDLES];
static jmp_buf s_exit;
static int s_exit_reason; /* 1: frame limit, 2: the game's exit */
static uint8_t *s_checkpoints;
static uint32_t s_checkpoint_count;
static int s_samples_decoded;
static uint32_t s_effect_end_tick;

#ifdef XL_TRACE
#define TRACE_RING 64
static uint32_t s_trace[TRACE_RING][3]; /* kind (0 enter, 1 return), address, A7 or target */
static uint32_t s_trace_next;

/* The functions entered: per entry the calls, the first and last frame, and
 * the phases it ran in (coverage.txt at the end). Phases: XL_PHASES, a list
 * of entries in hex (the game's stage routines); the phase is the one
 * entered last, 0 before any. */
#define COVERAGE_SLOTS 4096u
#define PHASES 31
static uint32_t s_coverage[COVERAGE_SLOTS][5]; /* entry + 1, calls, first, last, phases */
static uint32_t s_phase_entries[PHASES], s_phase_count, s_phase;

static void read_phases(void) {
  const char *p = getenv("XL_PHASES");
  while (p && *p && s_phase_count < PHASES) {
    char *end;
    uint32_t a = (uint32_t)strtoul(p, &end, 16);
    if (end == p) break;
    s_phase_entries[s_phase_count++] = a;
    p = *end ? end + 1 : end;
  }
}

static void write_coverage(const char *out_dir) {
  char path[1024];
  snprintf(path, sizeof path, "%s/coverage.txt", out_dir);
  FILE *f = fopen(path, "w");
  if (f == NULL) return;
  fprintf(f, "# entry calls first-frame last-frame phases (bit n: phase n, XL_PHASES)\n");
  for (uint32_t k = 0; k < COVERAGE_SLOTS; k++) {
    if (s_coverage[k][0]) {
      fprintf(f, "%06X %u %u %u %08X\n", s_coverage[k][0] - 1, s_coverage[k][1], s_coverage[k][2],
              s_coverage[k][3], s_coverage[k][4]);
    }
  }
  fclose(f);
}

void xl_trace_enter(uint32_t entry) {
  for (uint32_t n = 0; n < s_phase_count; n++) {
    if (s_phase_entries[n] == entry) s_phase = n + 1;
  }
  uint32_t k = (entry * 2654435761u) >> 20;
  while (s_coverage[k][0] && s_coverage[k][0] != entry + 1) k = (k + 1) % COVERAGE_SLOTS;
  if (s_coverage[k][0] == 0) {
    s_coverage[k][0] = entry + 1;
    s_coverage[k][2] = s_frame_count;
  }
  s_coverage[k][1]++;
  s_coverage[k][3] = s_frame_count;
  s_coverage[k][4] |= 1u << s_phase;
  uint32_t *t = s_trace[s_trace_next++ % TRACE_RING];
  t[0] = 0;
  t[1] = entry;
  t[2] = A(7);
}

void xl_trace_return(uint32_t at, uint32_t to) {
  uint32_t *t = s_trace[s_trace_next++ % TRACE_RING];
  t[0] = 1;
  t[1] = at;
  t[2] = to;
}
#endif

void xl_fatal(const char *what, uint32_t address) {
  fprintf(stderr, "xl: %s at %06X (frame %u, A7 %06X)\n", what, address, s_frame_count, A(7));
#ifdef XL_TRACE
  for (uint32_t k = 0; k < TRACE_RING; k++) {
    uint32_t *t = s_trace[(s_trace_next + k) % TRACE_RING];
    if (t[0] == 0 && t[1] == 0) continue;
    fprintf(stderr, "  %s %06X %s %06X\n", t[0] ? "return at" : "enter", t[1],
            t[0] ? "to" : "A7", t[2]);
  }
#endif
  fprintf(stderr, "  D:");
  for (int n = 0; n < 8; n++) fprintf(stderr, " %08X", D(n));
  fprintf(stderr, "\n  A:");
  for (int n = 0; n < 8; n++) fprintf(stderr, " %08X", A(n));
  fprintf(stderr, "\n");
  fflush(stdout);
  exit(3);
}

void xl_divide_by_zero(uint32_t pc) { xl_fatal("divide by zero", pc); }
void xl_stop(uint32_t sr, uint32_t pc) {
  (void)sr;
  xl_fatal("STOP", pc);
}

uint32_t xl_call(uint32_t address) {
  address &= 0xFFFFFFu;
  uint32_t lo = 0, hi = xl_entry_count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (xl_entries[mid].address < address) {
      lo = mid + 1;
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

/* The hardware at absolute addresses: memory, as the port's shadows (the
 * frame waits that poll it are patched). */
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

/* --- The memory map (XL_MEMMAP builds) ----------------------------------- */

/* Every byte the program reads or writes, and per 256-byte block the writes
 * and the first and last frame of them: memmap.bin (a byte per address: 1
 * read, 2 written by the code, 4 written by the runtime for it: a file read,
 * the text clear) and memmap.txt (the blocks). The runtime's own reads (the
 * screens) are left out. */
#ifdef XL_MEMMAP
#define BLOCKS (XL_MEMORY >> 8)
static uint8_t *s_touched;
static uint32_t s_block_reads[BLOCKS], s_block_writes[BLOCKS];
static uint32_t s_block_first[BLOCKS], s_block_last[BLOCKS];
static int s_memmap_paused;

void xl_memmap_access(uint32_t a, int size, int write) {
  if (s_memmap_paused) return;
  a &= 0xFFFFFFu;
  uint32_t block = a >> 8;
  if (write) {
    if (s_block_writes[block]++ == 0) s_block_first[block] = s_frame_count;
    s_block_last[block] = s_frame_count;
  } else {
    s_block_reads[block]++;
  }
  uint8_t flag = write == 0 ? 1u : write == 1 ? 2u : 4u;
  for (int k = 0; k < size; k++) s_touched[(a + (uint32_t)k) & 0xFFFFFFu] |= flag;
}

static void memmap_range(uint32_t a, uint32_t n, int write) {
  for (uint32_t k = 0; k < n; k += 4) {
    xl_memmap_access(a + k, n - k < 4 ? (int)(n - k) : 4, write ? 2 : 0);
  }
}

static void write_memmap(const char *out_dir) {
  char path[1024];
  snprintf(path, sizeof path, "%s/memmap.bin", out_dir);
  FILE *f = fopen(path, "wb");
  if (f) {
    fwrite(s_touched, 1, XL_MEMORY, f);
    fclose(f);
  }
  snprintf(path, sizeof path, "%s/memmap.txt", out_dir);
  f = fopen(path, "w");
  if (f == NULL) return;
  fprintf(f, "# block reads writes first-written-frame last-written-frame\n");
  for (uint32_t block = 0; block < BLOCKS; block++) {
    if (s_block_reads[block] == 0 && s_block_writes[block] == 0) continue;
    fprintf(f, "%06X %u %u %d %d\n", block << 8, s_block_reads[block], s_block_writes[block],
            s_block_writes[block] ? (int)s_block_first[block] : -1,
            s_block_writes[block] ? (int)s_block_last[block] : -1);
  }
  fclose(f);
}
#define MEMMAP_PAUSE(on) (s_memmap_paused = (on))
#else
#define memmap_range(a, n, write) ((void)0)
#define write_memmap(out_dir) ((void)0)
#define MEMMAP_PAUSE(on) ((void)0)
#endif

/* --- Frames: the game's VBL, the checkpoints, the screen ------------------- */

static void run_vbl(void) {
  s_total_vbl++;
  uint32_t handler = rd32(0x118);
  if (handler == 0) return;
  /* A 68000 interrupt frame: the PC, then SR. The handler ends with RTE. */
  A(7) -= 4;
  wr32(A(7), 0x00FFFFF0u);
  A(7) -= 2;
  wr16(A(7), xl_get_sr());
  uint32_t r = xl_call(handler);
  if (r != 0x00FFFFF0u) xl_fatal("the VBL handler returned elsewhere", r);
}

static void write_screen(uint32_t frame);
static void write_snapshot(void);
static uint32_t s_snapshot_frame;

/* As the port's measurement runs: a screen is due every N frames, at the
 * frame wait that starts with the count at a multiple of N, and shows the
 * picture that wait renders (XSP_VSYNC: after its VBLs; WAIT_VBL: before). */
static uint32_t s_screen_due;

static void frame_wait_begin(void) {
  s_last_sprite_count = s_frame_sprite_count;
  s_frame_sprite_count = 0;
  if (s_screen_interval && s_frame_count && s_frame_count % s_screen_interval == 0) {
    s_screen_due = s_frame_count;
  }
}

static void render_picture(void) {
  if (s_screen_due) write_screen(s_screen_due);
  s_screen_due = 0;
}

static void finish(int reason);

static void frame_wait_end(void) {
  s_frame_count++;
  if (s_frame_count % CHECKPOINT_INTERVAL == 0) {
    uint8_t *c = s_checkpoints + 32u * s_checkpoint_count++;
    memset(c, 0, 32);
    c[0] = (uint8_t)(s_frame_count >> 24);
    c[1] = (uint8_t)(s_frame_count >> 16);
    c[2] = (uint8_t)(s_frame_count >> 8);
    c[3] = (uint8_t)s_frame_count;
    memcpy(c + 4, xl_mem + XL_PLAYER_SCORE, 4);
    memcpy(c + 8, xl_mem + XL_WORD_00088E6C, 2);
    memcpy(c + 10, xl_mem + XL_BACKGROUND_SCROLL_COUNTER, 2);
    c[12] = (uint8_t)(s_last_sprite_count >> 8);
    c[13] = (uint8_t)s_last_sprite_count;
    memcpy(c + 16, xl_mem + XL_PLAYER_INFO_STRUCT, 14);
  }
  if (s_frame_limit && s_frame_count >= s_frame_limit) finish(1);
}

/* XSP_VSYNC: D0.w = the frames this game frame lasts. Runs that many VBLs,
 * then returns the game's VBL counter in D0.w and clears it, as the port's
 * lockstep builds (every VBL at the same point of the game's frame). */
void xl_patch_xsp_vsync(void) {
  uint32_t vbls = D(0) & 0xFFFFu;
  frame_wait_begin();
  for (uint32_t k = 0; k < vbls; k++) run_vbl();
  render_picture();
  frame_wait_end();
  uint32_t counter = rd16(XL_VBL_VWAIT_COUNTER);
  D(0) = (D(0) & 0xFFFF0000u) | counter;
  wr16(XL_VBL_VWAIT_COUNTER, 0);
  xl.n = 0;
  xl.z = 1;
  xl.v = 0;
  xl.c = 0;
  if (s_snapshot_frame && s_frame_count >= s_snapshot_frame) {
    s_snapshot_frame = 0;
    write_snapshot();
  }
}

/* WAIT_VBL(ticks): ticks + 1 VBLs. D1.w holds the argument after it, as the
 * port leaves it. */
void xl_patch_wait_vbl(void) {
  uint32_t ticks = rd16(A(7) + 6);
  D(1) = (D(1) & 0xFFFF0000u) | ticks;
  frame_wait_begin();
  render_picture();
  if (!(ticks & 0x8000u)) {
    for (uint32_t k = 0; k <= ticks; k++) run_vbl();
  }
  frame_wait_end();
}

/* XSP_OUT: the game's sprite list (8 bytes a sprite) is the frame's; counted,
 * kept for the screen, and the list starts over. Returns 0. */
static uint8_t s_sprites[MAXIMUM_SPRITES * 8];
static uint32_t s_sprite_list_count;

/* Which patterns the sprite lists draw, and with which flips (bit n: the
 * attributes' bits 15-14 = n): pattern_use.bin at the end, 'CRSU' then a byte
 * a pattern (tools/xlat/rp_store.py converts those). */
#define PATTERNS 2048u
static uint8_t s_pattern_use[PATTERNS];

#ifdef XL_RP_RENDER
/* The RP's renderer (rp/src/xl_render.c) beside the host's (XL_RP_RENDER
 * builds, run_host.py --rp-art): the same frames in the ST's 16 colours, as
 * rpscreen_NNNNN.bin (the screens' format, 16 colours), to check it against
 * the host's own picture through the palette scheme. */
extern const xl_store_art_t xl_stage_art;
static xl_render_frame_t s_rp_frame;

static void rp_take_sprites(uint32_t count) {
  s_rp_frame.art = &xl_stage_art;
  xl_render_take_sprites(&s_rp_frame, XL_SPRITE_DATA_TABLE, count);
}
#endif

void xl_patch_xsp_out(void) {
  uint32_t count = (rd32(XL_CURRENT_SPRITE_DATA_ENTRY) - XL_SPRITE_DATA_TABLE) / 8u;
  if (count > MAXIMUM_SPRITES) count = MAXIMUM_SPRITES;
  s_frame_sprite_count = count;
  s_sprite_list_count = count;
  memcpy(s_sprites, xl_mem + XL_SPRITE_DATA_TABLE, count * 8u);
  for (uint32_t k = 0; k < count; k++) {
    const uint8_t *e = s_sprites + 8u * k;
    uint32_t pattern = ((uint32_t)e[4] << 8) | e[5];
    if (pattern < PATTERNS) s_pattern_use[pattern] |= (uint8_t)(1u << (e[6] >> 6));
  }
#ifdef XL_RP_RENDER
  rp_take_sprites(count);
#endif
  wr32(XL_CURRENT_SPRITE_DATA_ENTRY, XL_SPRITE_DATA_TABLE);
  D(0) = 0;
  xl.n = 0;
  xl.z = 1;
  xl.v = 0;
  xl.c = 0;
}

/* CLEAR_TEXT_PLANE: all four planes (the X68000 clears them in one pass of
 * the CRTC's simultaneous access, which plain memory does not do). */
void xl_patch_clear_text_planes(void) {
  memset(xl_mem + 0xE00000u, 0, 4u * 0x20000u);
  memmap_range(0xE00000u, 4u * 0x20000u, 1);
}

/* The sine and cosine tables, as the port loads them (sincos.dat). */
void xl_patch_sincos(void) {
  char path[1024];
  snprintf(path, sizeof path, "%s/sincos.dat", s_data_dir);
  FILE *f = fopen(path, "rb");
  if (f == NULL) xl_fatal("no sincos.dat in the data folder", XL_SIN_COS_TABLES_START);
  size_t n = fread(xl_mem + XL_SIN_COS_TABLES_START, 1, 32768, f);
  fclose(f);
  if (n != 32768) xl_fatal("sincos.dat is short", XL_SIN_COS_TABLES_START);
  memmap_range(XL_SIN_COS_TABLES_START, 32768, 1);
}

/* The samples are loaded: the port decodes them and rewrites the game's
 * table, each file's entries pointing at its decoded copy, twice as long
 * (bytes of PCM for bytes of ADPCM). The effects' timing uses those lengths. */
#define SAMPLE_ENTRIES 256
#define SAMPLE_ENTRY_SIZE 16
#define SE_LENGTH 2
#define SE_ADDRESS 6
#define DECODED_SAMPLES 0x400000u

void xl_patch_samples_loaded(void) {
  uint32_t original[SAMPLE_ENTRIES];
  uint32_t table = XL_PCM_SAMPLE_INFO_TABLE;
  for (int e = 0; e < SAMPLE_ENTRIES; e++) {
    uint32_t at = table + (uint32_t)e * SAMPLE_ENTRY_SIZE;
    original[e] = rd32(at + SE_LENGTH) ? rd32(at + SE_ADDRESS) : 0;
  }
  uint32_t next = DECODED_SAMPLES;
  for (int e = 0; e < SAMPLE_ENTRIES; e++) {
    if (original[e] == 0) continue;
    int first = 0;
    while (original[first] != original[e]) first++;
    uint32_t at = table + (uint32_t)e * SAMPLE_ENTRY_SIZE;
    if (first != e) {
      uint32_t from = table + (uint32_t)first * SAMPLE_ENTRY_SIZE;
      wr32(at + SE_LENGTH, rd32(from + SE_LENGTH));
      wr32(at + SE_ADDRESS, rd32(from + SE_ADDRESS));
      continue;
    }
    uint32_t length = rd32(at + SE_LENGTH) * 2u;
    wr32(at + SE_LENGTH, length);
    wr32(at + SE_ADDRESS, next);
    next += length;
  }
  s_samples_decoded = 1;
}

/* --- IOCS (TRAP #15), as the port serves it ----------------------------- */

static const uint32_t k_adpcm_rates_hz100[5] = {4000000u * 100u / 1024u, 4000000u * 100u / 768u,
                                                8000000u * 100u / 1024u, 8000000u * 100u / 768u,
                                                8000000u * 100u / 512u};
static uint8_t s_unimplemented_iocs[256];

static uint32_t s_calls_iocs[256], s_calls_linef[512]; /* the calls made, per number */

static void iocs(void) {
  uint32_t f = D(0) & 0xFFu;
  s_calls_iocs[f]++;
  switch (f) {
    case 0x04: /* _BITSNS: no key held */
      D(0) = 0;
      return;
    case 0x92: { /* _VC_R1: d1.w the new R1, -1 reads it; returns the previous */
      uint32_t prev = rd16(0xE82500u);
      D(0) = prev;
      if ((D(1) & 0xFFFFu) != 0xFFFFu) wr16(0xE82500u, D(1));
      return;
    }
    case 0x14: { /* _TPALET2 */
      uint32_t at = 0xE82200u + (D(1) & 0xFu) * 2u;
      D(0) = 0;
      if ((int32_t)D(2) < 0) {
        D(0) = rd16(at);
      } else {
        wr16(at, D(2));
      }
      return;
    }
    case 0x3B: /* _JOYGET: nothing pressed (active low) */
      D(0) = 0xFFFFFFFFu;
      return;
    case 0x60: { /* _ADPCMOUT: d1 = rate << 8 | channels, d2 = bytes */
      uint32_t channels = D(1) & 3u, length = D(2);
      if (s_samples_decoded && channels && (length >> 1)) {
        uint32_t rate = (D(1) & 0xFFFFu) >> 8;
        if (rate > 4) rate = 4;
        s_effect_end_tick = length * FRAME_TIMER_HZ100 / k_adpcm_rates_hz100[rate] + 1u + s_total_vbl;
      }
      D(0) = 0;
      return;
    }
    case 0x66: /* _ADPCMSNS: 2 while an effect plays */
      D(0) = (s_effect_end_tick && s_total_vbl < s_effect_end_tick) ? 2u : 0u;
      return;
    case 0x67: /* _ADPCMMOD: 0 stops */
      if (D(1) == 0) s_effect_end_tick = 0;
      D(0) = 0;
      return;
    case 0x7F: /* _ONTIME: fixed, so the replays stay deterministic */
      D(0) = 100u * 100u;
      D(1) = 0;
      return;
    case 0x81: /* _B_SUPER: already in supervisor mode */
      D(0) = 0xFFFFFFFFu;
      return;
    case 0xCE: { /* _BGTEXTGT: d1 page, d2 x, d3 y: the map's word */
      uint32_t at = 0xEB8000u + 0x4000u + (D(1) & 1u) * 0x2000u +
                    ((((D(3) & 63u) << 6) + (D(2) & 63u)) & 0xFFFFu) * 2u;
      D(0) = rd16(at);
      return;
    }
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
    case 0x10: case 0x20: case 0x22: case 0x23: case 0x7D: case 0x87: case 0x90:
    case 0xAE: case 0xAF: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
    case 0xC1: case 0xC2: case 0xCA:
      D(0) = 0;
      return;
    default:
      if (!s_unimplemented_iocs[f]) {
        fprintf(stderr, "xl: IOCS $%02X not implemented (returns 0)\n", f);
        s_unimplemented_iocs[f] = 1;
      }
      D(0) = 0;
      return;
  }
}

void xl_trap(uint32_t n, uint32_t pc) {
  if (n == 15) {
    iocs();
  } else if (n == 2 || n == 4) {
    /* PCM8 (mode reads), MCDRV (no driver: never called) */
  } else {
    xl_fatal("TRAP", pc);
  }
}

/* --- Human68k DOS and FLOAT (line F), as the port serves them -------------- */

static void host_path(char *out, size_t size, uint32_t name) {
  size_t n = (size_t)snprintf(out, size, "%s/", s_data_dir);
  for (uint32_t k = 0; n + 1 < size; k++) {
    uint8_t ch = rd8(name + k);
    if (ch == 0) break;
    out[n++] = ch == '\\' ? '/' : (char)ch;
  }
  out[n] = 0;
}

static FILE **handle_slot(uint32_t handle) {
  int32_t h = (int32_t)(int16_t)handle - FIRST_FILE_HANDLE;
  if (h < 0 || h >= FILE_HANDLES || s_files[h] == NULL) return NULL;
  return &s_files[h];
}

static uint32_t open_file(uint32_t name, const char *mode) {
  int slot = 0;
  while (slot < FILE_HANDLES && s_files[slot] != NULL) slot++;
  if (slot == FILE_HANDLES) return (uint32_t)-4;
  char path[1024];
  host_path(path, sizeof path, name);
  FILE *f = fopen(path, mode);
  if (f == NULL) return (uint32_t)-2;
  s_files[slot] = f;
  return (uint32_t)(slot + FIRST_FILE_HANDLE);
}

static uint32_t nameck(uint32_t name, uint32_t buffer) {
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
  return 0;
}

static uint8_t s_unimplemented_dos[512];

void xl_linef(uint32_t word, uint32_t pc) {
  s_calls_linef[word & 0x1FFu]++;
  uint32_t args = A(7);
  uint32_t r = 0;
  switch (word) {
    case 0xFE00: /* __LMUL */
      r = (uint32_t)((int32_t)D(0) * (int32_t)D(1));
      break;
    case 0xFE01: /* __LDIV */
      if (D(1) == 0) xl_divide_by_zero(pc);
      r = (uint32_t)((int32_t)D(0) / (int32_t)D(1));
      break;
    case 0xFE0D: case 0xFF0D: case 0xFF20: case 0xFF23: case 0xFF25: case 0xFF21: case 0xFF44:
      r = 0;
      break;
    case 0xFF00: case 0xFF4C: /* _EXIT, _EXIT2 */
      finish(2);
      break;
    case 0xFF06: /* _INPOUT: no key */
      r = 0;
      break;
    case 0xFF09: { /* _PRINT */
      for (uint32_t p = rd32(args); rd8(p); p++) putchar((int)rd8(p));
      r = 0;
      break;
    }
    case 0xFF36: /* _DSKFRE */
      r = 10000000u;
      break;
    case 0xFF37: /* _NAMECK */
      r = nameck(rd32(args), rd32(args + 4));
      break;
    case 0xFF3C: /* _CREATE */
      r = open_file(rd32(args), "w+b");
      break;
    case 0xFF3D: /* _OPEN: an existing file, for reading and writing */
      r = open_file(rd32(args), "r+b");
      break;
    case 0xFF3E: { /* _CLOSE */
      FILE **f = handle_slot(rd16(args));
      if (f == NULL) {
        r = (uint32_t)-6;
      } else {
        fclose(*f);
        *f = NULL;
        r = 0;
      }
      break;
    }
    case 0xFF3F: case 0xFF40: { /* _READ, _WRITE */
      FILE **f = handle_slot(rd16(args));
      uint32_t buffer = rd32(args + 2), length = rd32(args + 6) & 0x7FFFFFFFu;
      if (f == NULL) {
        r = (uint32_t)-6;
      } else if (word == 0xFF3F) {
        r = (uint32_t)fread(xl_mem + (buffer & 0xFFFFFFu), 1, length, *f);
        memmap_range(buffer, r, 1);
      } else {
        memmap_range(buffer, length, 0);
        r = (uint32_t)fwrite(xl_mem + (buffer & 0xFFFFFFu), 1, length, *f);
      }
      break;
    }
    case 0xFF41: { /* _DELETE */
      char path[1024];
      host_path(path, sizeof path, rd32(args));
      r = remove(path) == 0 ? 0 : (uint32_t)-2;
      break;
    }
    case 0xFF42: { /* _SEEK: the new position */
      FILE **f = handle_slot(rd16(args));
      int whence[4] = {SEEK_SET, SEEK_CUR, SEEK_END, SEEK_END};
      if (f == NULL) {
        r = (uint32_t)-6;
      } else if (fseek(*f, (long)(int32_t)rd32(args + 2), whence[rd16(args + 6) & 3u]) != 0) {
        r = (uint32_t)-1;
      } else {
        r = (uint32_t)ftell(*f);
      }
      break;
    }
    case 0xFF4A: { /* _SETBLOCK */
      uint32_t largest = HEAP_ADDRESS + HEAP_SIZE - rd32(args);
      r = rd32(args + 4) <= largest ? 0 : (largest | 0x81000000u);
      break;
    }
    case 0xFF51: /* _GETPDB */
      r = HEAP_ADDRESS + 16u;
      break;
    default:
      if (!s_unimplemented_dos[word & 0x1FFu]) {
        fprintf(stderr, "xl: DOS/FLOAT $%04X not implemented (returns -1) at %06X\n", word, pc);
        s_unimplemented_dos[word & 0x1FFu] = 1;
      }
      r = (uint32_t)-1;
      break;
  }
  D(0) = r;
  /* The port returns from a call with the flags of a move of the result. */
  xl.n = (r >> 31) & 1u;
  xl.z = r == 0;
  xl.v = 0;
  xl.c = 0;
}

/* --- The screen: the port's dump format (tools/re/oracle.py) ---------------- */

/* The X68000's picture as the port draws it (graphics.s): the two graphics
 * pages (page 0 in front, the backdrop where both are clear), then the text
 * and the sprites in the order R1 gives them. 256 x 256 pixels, inside a
 * guard of 16 the sprites may reach into. */
#define SCREEN_WIDTH 256
#define SCREEN_LINES 256
#define GUARD 16
#define RENDER_STRIDE (GUARD + SCREEN_WIDTH + GUARD)
#define RENDER_LINES (GUARD + SCREEN_LINES + GUARD)
#define PCG_PATTERNS 0x75Eu
#define GVRAM 0xC00000u
#define GVRAM_PAGE 0x80000u
#define TEXT_VRAM 0xE00000u
#define TEXT_PLANE 0x20000u
#define CRTC 0xE80000u
#define GRAPHICS_PALETTE 0xE82000u
#define SPRITE_PALETTE 0xE82200u
#define VC_R1 0xE82500u

/* What each pixel shows: an entry of the X68000's palettes, 1..255 the
 * graphics palette's, ENTRY_SPRITE + n the sprite and text palette's (its
 * entry 0 the backdrop); the colour follows from the entry. */
#define ENTRY_SPRITE 256u
#define ENTRIES 512u
static uint16_t s_render[RENDER_LINES][RENDER_STRIDE];

/* GRB555 + intensity (ignored); each component 5 to 8 bits as the port. */
static uint32_t colour_rgb(uint32_t word) {
  uint32_t g = (word >> 11) & 31u, r = (word >> 6) & 31u, b = (word >> 1) & 31u;
  r = (r << 3) | (r >> 2);
  g = (g << 3) | (g >> 2);
  b = (b << 3) | (b >> 2);
  return (r << 16) | (g << 8) | b;
}

static uint32_t entry_rgb(uint32_t entry) {
  return colour_rgb(rd16(entry < ENTRY_SPRITE ? GRAPHICS_PALETTE + entry * 2u
                                              : SPRITE_PALETTE + (entry - ENTRY_SPRITE) * 2u));
}

static void render_graphics(void) {
  uint32_t x0 = rd16(CRTC + 0x18), y0 = rd16(CRTC + 0x1A);
  uint32_t x1 = rd16(CRTC + 0x20), y1 = rd16(CRTC + 0x22);
  for (uint32_t y = 0; y < SCREEN_LINES; y++) {
    uint16_t *out = &s_render[GUARD + y][GUARD];
    uint32_t line0 = GVRAM + ((y0 + y) & 511u) * 1024u;
    uint32_t line1 = GVRAM + GVRAM_PAGE + ((y1 + y) & 511u) * 1024u;
    for (uint32_t x = 0; x < SCREEN_WIDTH; x++) {
      uint32_t index = rd8(line0 + ((x0 + x) & 511u) * 2u + 1u);
      if (index == 0) index = rd8(line1 + ((x1 + x) & 511u) * 2u + 1u);
      out[x] = (uint16_t)(index ? index : ENTRY_SPRITE);
    }
  }
}

/* Four planes of 1024 x 1024, palette block 0, colour 0 clear; the
 * horizontal scroll in whole bytes, as the port applies it. */
static void render_text(void) {
  uint32_t column = rd16(CRTC + 0x14) >> 3, top = rd16(CRTC + 0x16);
  for (uint32_t y = 0; y < SCREEN_LINES; y++) {
    uint32_t line = TEXT_VRAM + ((top + y) & 1023u) * 128u;
    uint16_t *out = &s_render[GUARD + y][GUARD];
    for (uint32_t b = 0; b < SCREEN_WIDTH / 8; b++) {
      uint32_t at = line + ((column + b) & 127u);
      uint32_t p0 = rd8(at), p1 = rd8(at + TEXT_PLANE), p2 = rd8(at + 2 * TEXT_PLANE),
               p3 = rd8(at + 3 * TEXT_PLANE);
      if ((p0 | p1 | p2 | p3) == 0) continue;
      for (uint32_t k = 0; k < 8; k++) {
        uint32_t bit = 7u - k;
        uint32_t c = (((p3 >> bit) & 1u) << 3) | (((p2 >> bit) & 1u) << 2) |
                     (((p1 >> bit) & 1u) << 1) | ((p0 >> bit) & 1u);
        if (c) out[b * 8u + k] = (uint16_t)(ENTRY_SPRITE + c);
      }
    }
  }
}

/* 16 x 16 at 4 bits a pixel: the left 8 pixels of row r at r * 4, the right
 * 8 at 64 + r * 4. Attributes: bits 4-0 the priority (buckets drawn in
 * ascending order, a bucket's sprites in reverse list order), 11-8 the
 * palette block, 14 and 15 the horizontal and vertical flips. */
static void render_sprites(void) {
  uint32_t patterns = rd32(XL_SPRITE_DATA_ADDRESS);
  if (patterns == 0) return;
  for (uint32_t bucket = 0; bucket < 32; bucket++) {
    for (uint32_t i = s_sprite_list_count; i-- > 0;) {
      const uint8_t *e = s_sprites + i * 8u;
      uint32_t x = ((uint32_t)e[0] << 8) | e[1], y = ((uint32_t)e[2] << 8) | e[3];
      uint32_t pattern = ((uint32_t)e[4] << 8) | e[5], attr = ((uint32_t)e[6] << 8) | e[7];
      if ((attr & 31u) != bucket) continue;
      if (pattern >= PCG_PATTERNS || x > RENDER_STRIDE - 16 || y > RENDER_LINES - 16) continue;
      uint32_t data = patterns + pattern * 128u;
      uint32_t block = ENTRY_SPRITE + ((attr >> 8) & 15u) * 16u;
      for (uint32_t r = 0; r < 16; r++) {
        uint32_t row = (attr & 0x8000u) ? 15u - r : r;
        uint32_t left = rd32(data + row * 4u), right = rd32(data + 64u + row * 4u);
        uint16_t *out = &s_render[y + r][x];
        for (uint32_t k = 0; k < 16; k++) {
          uint32_t c = k < 8 ? (left >> (28u - 4u * k)) & 15u : (right >> (28u - 4u * (k - 8u))) & 15u;
          if (c) out[(attr & 0x4000u) ? 15u - k : k] = (uint16_t)(block + c);
        }
      }
    }
  }
}

/* entries_NNNNN.bin (--entries): 'CRSE', the two palettes as the program
 * holds them (512 big-endian words: the graphics palette's 256, then the
 * sprite and text palette's), then each pixel's entry (s_render's
 * numbering), 256 x 256 little-endian words. What the RP's palette scheme
 * is fitted on (tools/xlat/palette_fit.py). */
static int s_entry_dumps;

static void write_entries(uint32_t frame) {
  char path[1024];
  snprintf(path, sizeof path, "%s/entries_%05u.bin", s_out_dir, frame);
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    perror(path);
    return;
  }
  fwrite("CRSE", 1, 4, f);
  fwrite(xl_mem + GRAPHICS_PALETTE, 1, 2u * ENTRIES, f);
  for (uint32_t y = 0; y < SCREEN_LINES; y++) {
    uint8_t line[2 * SCREEN_WIDTH];
    for (uint32_t x = 0; x < SCREEN_WIDTH; x++) {
      uint16_t e = s_render[GUARD + y][GUARD + x];
      line[2 * x] = (uint8_t)e;
      line[2 * x + 1] = (uint8_t)(e >> 8);
    }
    fwrite(line, 1, sizeof line, f);
  }
  fclose(f);
}

#ifdef XL_RP_RENDER
static void write_rp_screen(uint32_t frame) {
  static uint8_t fb[XL_RENDER_W * XL_RENDER_H];
  uint16_t st[16];
  s_rp_frame.art = &xl_stage_art;
  xl_render_prepare(&s_rp_frame);
  xl_render_band(fb, &s_rp_frame, 0, XL_RENDER_H);
  xl_render_palette(&xl_stage_art, st);
  char path[1024];
  snprintf(path, sizeof path, "%s/rpscreen_%05u.bin", s_out_dir, frame);
  FILE *f = fopen(path, "wb");
  if (f == NULL) return;
  uint8_t head[12] = {'C', 'R', 'S', 'S', 1, 0, 1, 0, 0, 16, 0, 0};
  fwrite(head, 1, sizeof head, f);
  for (uint32_t k = 0; k < 256; k++) {
    uint8_t rgb[3] = {0, 0, 0};
    for (uint32_t c = 0; k < 16 && c < 3; c++) {
      rgb[c] = (uint8_t)((((st[k] >> (8 - 4 * c)) & 7u) * 255u + 3u) / 7u);
    }
    fwrite(rgb, 1, 3, f);
  }
  fwrite(fb, 1, sizeof fb, f);
  fclose(f);
}
#endif

static void write_screen(uint32_t frame) {
  MEMMAP_PAUSE(1);
  if (getenv("XL_GVRAM_WATCH")) {
    static uint32_t last;
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; k < 2 * GVRAM_PAGE; k++) h = (h ^ xl_mem[GVRAM + k]) * 16777619u;
    if (h != last) fprintf(stderr, "gvram %u %08x crtc %04x %04x %04x %04x r1 %04x\n", frame, h,
                           rd16(CRTC + 0x18), rd16(CRTC + 0x1A), rd16(CRTC + 0x20), rd16(CRTC + 0x22), rd16(VC_R1));
    last = h;
  }
  render_graphics();
  uint32_t r1 = rd16(VC_R1);
  if (((r1 >> 12) & 3u) > ((r1 >> 10) & 3u)) { /* text in front */
    render_sprites();
    render_text();
  } else {
    render_text();
    render_sprites();
  }

  if (s_entry_dumps) write_entries(frame);
#ifdef XL_RP_RENDER
  write_rp_screen(frame);
#endif

  static uint8_t pixels[SCREEN_WIDTH * SCREEN_LINES];
  uint32_t palette[256];
  uint32_t colours = 0, overflow = 0;
  uint32_t rgb_of[ENTRIES];
  for (uint32_t e = 0; e < ENTRIES; e++) rgb_of[e] = entry_rgb(e);
  MEMMAP_PAUSE(0);
  for (uint32_t y = 0; y < SCREEN_LINES; y++) {
    for (uint32_t x = 0; x < SCREEN_WIDTH; x++) {
      uint32_t rgb = rgb_of[s_render[GUARD + y][GUARD + x]], k = 0;
      while (k < colours && palette[k] != rgb) k++;
      if (k == colours) {
        if (colours < 256) {
          palette[colours++] = rgb;
        } else {
          k = 255;
          overflow++;
        }
      }
      pixels[y * SCREEN_WIDTH + x] = (uint8_t)k;
    }
  }
  if (overflow) fprintf(stderr, "xl: screen %u has more than 256 colours\n", frame);

  char path[1024];
  snprintf(path, sizeof path, "%s/screen_%05u.bin", s_out_dir, frame);
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    perror(path);
    return;
  }
  uint8_t head[12] = {'C', 'R', 'S', 'S', SCREEN_WIDTH >> 8, SCREEN_WIDTH & 255,
                      SCREEN_LINES >> 8, SCREEN_LINES & 255, (uint8_t)(colours >> 8), (uint8_t)colours, 0, 0};
  fwrite(head, 1, sizeof head, f);
  for (uint32_t k = 0; k < 256; k++) {
    uint8_t rgb[3] = {0, 0, 0};
    if (k < colours) {
      rgb[0] = (uint8_t)(palette[k] >> 16);
      rgb[1] = (uint8_t)(palette[k] >> 8);
      rgb[2] = (uint8_t)palette[k];
    }
    fwrite(rgb, 1, 3, f);
  }
  fwrite(pixels, 1, sizeof pixels, f);
  fclose(f);
}

/* --- Snapshots: the game resumed where it waited for a frame -------------- */

/* Taken at the end of XSP_VSYNC's wait, so a resumed run goes on at the
 * instruction the patch continues at (xlat.py PATCHES), returning into the
 * callers through the return addresses on the 68000 stack. snapshot.bin:
 * 'XLSN', the resume address, the CPU (d0-d7, a0-a7, n z v c x, the system
 * byte), the frame and VBL counts, the sprite counts, the effect's end tick,
 * whether the samples are decoded, the sprite list's count, the checkpoints'
 * count (all u32 little-endian), the sprite list, the checkpoints so far,
 * then the 16 MB of memory. */
#define XSP_VSYNC_RESUME 0x02021Au
#define SNAPSHOT_WORDS 32

static void write_snapshot(void) {
  for (int k = 0; k < FILE_HANDLES; k++) {
    if (s_files[k]) xl_fatal("a snapshot with a file open", XSP_VSYNC_RESUME);
  }
  MEMMAP_PAUSE(1);
  uint32_t w[SNAPSHOT_WORDS], n = 0;
  w[n++] = 0x4E534C58u; /* 'XLSN' */
  w[n++] = XSP_VSYNC_RESUME;
  for (int k = 0; k < 8; k++) w[n++] = xl.d[k];
  for (int k = 0; k < 8; k++) w[n++] = xl.a[k];
  w[n++] = xl.n;
  w[n++] = xl.z;
  w[n++] = xl.v;
  w[n++] = xl.c;
  w[n++] = xl.x;
  w[n++] = xl.sr_high;
  w[n++] = s_frame_count;
  w[n++] = s_total_vbl;
  w[n++] = s_frame_sprite_count;
  w[n++] = s_last_sprite_count;
  w[n++] = s_effect_end_tick;
  w[n++] = (uint32_t)s_samples_decoded;
  w[n++] = s_sprite_list_count;
  w[n++] = s_checkpoint_count;
  char path[1024];
  snprintf(path, sizeof path, "%s/snapshot.bin", s_out_dir);
  FILE *f = fopen(path, "wb");
  if (f == NULL) xl_fatal("cannot write snapshot.bin", XSP_VSYNC_RESUME);
  for (uint32_t k = 0; k < n; k++) {
    uint8_t b[4] = {(uint8_t)w[k], (uint8_t)(w[k] >> 8), (uint8_t)(w[k] >> 16), (uint8_t)(w[k] >> 24)};
    fwrite(b, 1, 4, f);
  }
  fwrite(s_sprites, 1, sizeof s_sprites, f);
  fwrite(s_checkpoints, 32, s_checkpoint_count, f);
  fwrite(xl_mem, 1, XL_MEMORY, f);
  fclose(f);

  /* Every long above the stack pointer (the game's stack is its own, in the
   * heap: 64 KB of it) that could be a return address into the code;
   * xlat.py keeps those that follow a call. */
  snprintf(path, sizeof path, "%s/resume.txt", s_out_dir);
  f = fopen(path, "w");
  if (f == NULL) xl_fatal("cannot write resume.txt", XSP_VSYNC_RESUME);
  fprintf(f, "%06X # where the run resumes\n", XSP_VSYNC_RESUME);
  for (uint32_t at = A(7); at + 4 <= A(7) + 0x10000u && at + 4 <= XL_MEMORY; at += 2) {
    uint32_t v = rd32(at);
    if (v >= XL_LOAD && v < XL_BSS && !(v & 1u)) fprintf(f, "?%06X # at %06X\n", v, at);
  }
  fclose(f);
  printf("xl: snapshot at frame %u\n", s_frame_count);
  MEMMAP_PAUSE(0);
}

static uint32_t read_snapshot(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    perror(path);
    exit(1);
  }
  uint8_t b[SNAPSHOT_WORDS * 4];
  uint32_t w[SNAPSHOT_WORDS], n = 0;
  if (fread(b, 1, sizeof b, f) != sizeof b) xl_fatal("a short snapshot", 0);
  for (int k = 0; k < SNAPSHOT_WORDS; k++) {
    w[k] = b[4 * k] | ((uint32_t)b[4 * k + 1] << 8) | ((uint32_t)b[4 * k + 2] << 16) |
           ((uint32_t)b[4 * k + 3] << 24);
  }
  if (w[n++] != 0x4E534C58u) xl_fatal("not a snapshot", 0);
  uint32_t resume = w[n++];
  for (int k = 0; k < 8; k++) xl.d[k] = w[n++];
  for (int k = 0; k < 8; k++) xl.a[k] = w[n++];
  xl.n = w[n++];
  xl.z = w[n++];
  xl.v = w[n++];
  xl.c = w[n++];
  xl.x = w[n++];
  xl.sr_high = w[n++];
  s_frame_count = w[n++];
  s_total_vbl = w[n++];
  s_frame_sprite_count = w[n++];
  s_last_sprite_count = w[n++];
  s_effect_end_tick = w[n++];
  s_samples_decoded = (int)w[n++];
  s_sprite_list_count = w[n++];
  s_checkpoint_count = w[n++];
  if (fread(s_sprites, 1, sizeof s_sprites, f) != sizeof s_sprites ||
      fread(s_checkpoints, 32, s_checkpoint_count, f) != s_checkpoint_count ||
      fread(xl_mem, 1, XL_MEMORY, f) != XL_MEMORY) {
    xl_fatal("a short snapshot", 0);
  }
  fclose(f);
  return resume;
}

/* --- The run --------------------------------------------------------------- */

static void finish(int reason) {
  s_exit_reason = reason;
  longjmp(s_exit, 1);
}

static void write_pattern_use(void) {
  char path[1024];
  snprintf(path, sizeof path, "%s/pattern_use.bin", s_out_dir);
  FILE *f = fopen(path, "wb");
  if (f == NULL) return;
  fwrite("CRSU", 1, 4, f);
  fwrite(s_pattern_use, 1, sizeof s_pattern_use, f);
  fclose(f);
}

static void write_checkpoints(void) {
  char path[1024];
  snprintf(path, sizeof path, "%s/checkpoints.bin", s_out_dir);
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    perror(path);
    return;
  }
  uint8_t head[8] = {'C', 'R', 'S', 'C', (uint8_t)(s_checkpoint_count >> 24),
                     (uint8_t)(s_checkpoint_count >> 16), (uint8_t)(s_checkpoint_count >> 8),
                     (uint8_t)s_checkpoint_count};
  fwrite(head, 1, 8, f);
  fwrite(s_checkpoints, 32, s_checkpoint_count, f);
  fclose(f);
}

/* graphics.bin as the port's measurement runs write it: 'CRSG', GVRAM pages
 * 0-1, the palettes, the per-pattern palette usage (not kept: zeros), the
 * sprite patterns. */
static void write_graphics(void) {
  char path[1024];
  snprintf(path, sizeof path, "%s/graphics.bin", s_out_dir);
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    perror(path);
    return;
  }
  static const uint8_t usage[2048 * 2];
  fwrite("CRSG", 1, 4, f);
  fwrite(xl_mem + GVRAM, 1, 2 * GVRAM_PAGE, f);
  fwrite(xl_mem + GRAPHICS_PALETTE, 1, 0x400, f);
  fwrite(usage, 1, sizeof usage, f);
  fwrite(xl_mem + (rd32(XL_SPRITE_DATA_ADDRESS) & 0xFFFFFFu), 1, PCG_PATTERNS * 128u, f);
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s DATA_DIR FRAMES OUT_DIR [--screens N]\n", argv[0]);
    return 2;
  }
  s_data_dir = argv[1];
  s_frame_limit = (uint32_t)strtoul(argv[2], NULL, 10);
  s_out_dir = argv[3];
  const char *resume_path = NULL;
  for (int k = 4; k + 1 < argc; k++) {
    if (strcmp(argv[k], "--screens") == 0) s_screen_interval = (uint32_t)strtoul(argv[k + 1], NULL, 10);
    if (strcmp(argv[k], "--snapshot") == 0) s_snapshot_frame = (uint32_t)strtoul(argv[k + 1], NULL, 10);
    if (strcmp(argv[k], "--entries") == 0) s_entry_dumps = atoi(argv[k + 1]);
    if (strcmp(argv[k], "--resume") == 0) resume_path = argv[k + 1];
  }
  xl_mem = calloc(XL_MEMORY, 1);
  s_checkpoints = calloc(s_frame_limit / CHECKPOINT_INTERVAL + 2, 32);
  if (xl_mem == NULL || s_checkpoints == NULL) return 1;
#ifdef XL_MEMMAP
  s_touched = calloc(XL_MEMORY, 1);
  if (s_touched == NULL) return 1;
#endif

  char path[1024];
  snprintf(path, sizeof path, "%s/xl_image.bin", s_out_dir);
  FILE *image = fopen(path, "rb");
  if (image == NULL) {
    perror(path);
    return 1;
  }
  size_t got = fread(xl_mem + XL_LOAD, 1, XL_BSS - XL_LOAD, image);
  fclose(image);
  if (got != XL_BSS - XL_LOAD) {
    fprintf(stderr, "%s: %zu bytes, %u expected\n", path, got, XL_BSS - XL_LOAD);
    return 1;
  }

  /* As Human68k starts a process (and the port does): a0 the memory block,
   * a1 the program's end, a2 the command line, a3 the environment, a4 the
   * entry; all into the cleared process area. The stack: the game's own. */
  A(0) = A(1) = A(2) = A(3) = HEAP_ADDRESS;
  A(4) = XL_ENTRY;
  A(7) = XL_NEW_STACK;
  xl.sr_high = 0x20; /* supervisor */

#ifdef XL_TRACE
  read_phases();
#endif
  uint32_t start = XL_ENTRY;
  if (resume_path) start = read_snapshot(resume_path);

  if (setjmp(s_exit) == 0) {
    uint32_t r = xl_call(start);
    for (;;) r = xl_call(r); /* code the program returned into */
  }
  write_checkpoints();
  write_pattern_use();
  MEMMAP_PAUSE(1);
  write_graphics();
  write_memmap(s_out_dir);
  for (uint32_t k = 0; k < 256; k++) {
    if (s_calls_iocs[k]) printf("xl: IOCS $%02X %u calls\n", k, s_calls_iocs[k]);
  }
  for (uint32_t k = 0; k < 512; k++) {
    if (s_calls_linef[k]) printf("xl: $%04X %u calls\n", 0xFE00u + k, s_calls_linef[k]);
  }
#ifdef XL_TRACE
  write_coverage(s_out_dir);
#endif
  printf("xl: %s after %u frames, %u checkpoints, %u VBLs\n",
         s_exit_reason == 1 ? "frame limit" : "the game exited", s_frame_count,
         s_checkpoint_count, s_total_vbl);
  return 0;
}
