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
#include <math.h>
#include <setjmp.h>
#include <signal.h>
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

/* What each file read put where (the music calls name a song by its data's
 * address): the file's path in the data folder, the buffer, the length. */
#define LOADS 128
static char s_file_names[FILE_HANDLES][96];
static struct {
  uint32_t buffer, length;
  char name[96];
} s_loads[LOADS];
static uint32_t s_load_count;

static jmp_buf s_exit;
static int s_exit_reason; /* 1: frame limit, 2: the game's exit */
static uint8_t *s_checkpoints;
static uint32_t s_checkpoint_count;
static int s_samples_decoded;
static uint32_t s_effect_end_tick;
static FILE *s_effects_log; /* the effects' starts and stops (XL_MUSIC) */

/* The patterns the current phase's sprites drew (PATTERNS of them, below):
 * counted at each phase change (XL_TRACE, XL_PHASES). */
static uint8_t s_phase_patterns[2048];

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
/* The patterns each phase's sprites drew, as pattern_use.bin records them
 * (pattern_use_pNN.bin at the end, NN the phase): a stage's own patterns. */
static uint8_t s_phase_use[PHASES + 1][2048];
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

#ifdef XL_MEMMAP
static void memmap_phase_end(uint32_t phase);
#endif

/* XL_SCENES=FILE (tools/xlat/rp_store.py's scenes.txt): the RP's scene
 * changes, modelled. Each scene keeps its RAM pages in pool slots chosen once
 * for every scene; entering a scene, a page marked ENTRY whose slot holds
 * another page (or none yet) is set to its content after the boot (the
 * file's memory dump), as the RP's change does; the first scene's are the
 * start's. A run with it against one without tells whether
 * the pages lost that way matter to the game. The file: "memory PATH",
 * "phase N SCENE" and "page SCENE PAGE SLOT ENTRY" lines. */
#define SCENES_MAX 16
#define SCENE_PAGES 512
#define POOL_SLOTS 1024
static struct {
  uint16_t page[SCENE_PAGES], slot[SCENE_PAGES];
  uint8_t entry[SCENE_PAGES];
  uint32_t count;
} s_scenes[SCENES_MAX];
static int s_scene_of_phase[PHASES + 1];
static uint8_t *s_scene_init;
static uint32_t s_slot_owner[POOL_SLOTS];
static int s_scene = -1, s_scenes_on;

/* Where each of a scene's pages is on the RP (the file's "page" and "kind
 * lines"), so the model can report an access the RP would get wrong: a write
 * into a page it keeps in flash (lost), a read of one whose content is not
 * the images' any more (stale: written by another scene, or loaded from a
 * file), an access to the spare page every
 * page no run touched shares (another stray page's bytes), a sink read (its
 * writes go to the spare page), the text plane past the 32 bytes of a line
 * the RP keeps. Each (scene, page, kind of problem) once, as it happens:
 * "xl: outside" lines, and outside.txt at the end. */
enum { PAGE_SPARE, PAGE_RAM, PAGE_FLASH, PAGE_TEXT, PAGE_SINK };
enum { OUT_FLASH_WRITE = 1, OUT_SPARE_READ = 2, OUT_SPARE_WRITE = 4, OUT_SINK_READ = 8,
       OUT_TEXT_PAST = 16, OUT_FLASH_STALE = 32 };
static uint8_t s_page_kind[SCENES_MAX][XL_MEMORY >> 8];
/* Written since its last comparison with the memory the images hold (the
 * file's dump): a flash page read with other content is stale on the RP. */
static uint8_t s_page_dirty[XL_MEMORY >> 8];
static uint8_t s_outside_seen[SCENES_MAX][XL_MEMORY >> 8];
#define OUTSIDE_NOTES 4096
static struct {
  uint32_t scene, frame, address, kind, function;
} s_outside[OUTSIDE_NOTES];
static uint32_t s_outside_count;

static void scenes_read(void) {
  const char *path = getenv("XL_SCENES");
  for (uint32_t k = 0; k <= PHASES; k++) s_scene_of_phase[k] = -1;
  if (path == NULL) return;
  FILE *f = fopen(path, "r");
  if (f == NULL) xl_fatal("no XL_SCENES file", 0);
  char line[1200], name[1024];
  unsigned a, b, c, d;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "memory %1023s", name) == 1) {
      FILE *m = fopen(name, "rb");
      s_scene_init = malloc(XL_MEMORY);
      if (m == NULL || s_scene_init == NULL || fread(s_scene_init, 1, XL_MEMORY, m) != XL_MEMORY) {
        xl_fatal("XL_SCENES: no memory dump", 0);
      }
      fclose(m);
    } else if (sscanf(line, "phase %u %u", &a, &b) == 2 && a <= PHASES && b < SCENES_MAX) {
      s_scene_of_phase[a] = (int)b;
    } else if (sscanf(line, "page %u %u %u %u", &a, &b, &c, &d) == 4 && a < SCENES_MAX &&
               s_scenes[a].count < SCENE_PAGES && c < POOL_SLOTS) {
      uint32_t k = s_scenes[a].count++;
      s_scenes[a].page[k] = (uint16_t)b;
      s_scenes[a].slot[k] = (uint16_t)c;
      s_scenes[a].entry[k] = (uint8_t)d;
      s_page_kind[a][b & ((XL_MEMORY >> 8) - 1u)] = PAGE_RAM;
    } else if (sscanf(line, "kind %u %15s %u %u", &a, name, &b, &c) == 4 && a < SCENES_MAX &&
               c < (XL_MEMORY >> 8)) {
      uint8_t kind = strcmp(name, "flash") == 0 ? PAGE_FLASH
                     : strcmp(name, "text") == 0 ? PAGE_TEXT
                     : strcmp(name, "sink") == 0 ? PAGE_SINK : PAGE_SPARE;
      for (uint32_t p = b; p <= c; p++) s_page_kind[a][p] = kind;
    }
  }
  fclose(f);
  if (s_scene_init == NULL) xl_fatal("XL_SCENES: no memory line", 0);
  for (uint32_t k = 0; k < POOL_SLOTS; k++) s_slot_owner[k] = 0xFFFFFFFFu;
  s_scenes_on = 1;
}

static void scene_enter(int scene) {
  uint32_t set = 0, changed = 0;
  char list[256] = "";
  for (uint32_t k = 0; k < s_scenes[scene].count; k++) {
    uint32_t page = s_scenes[scene].page[k], slot = s_scenes[scene].slot[k];
    if (s_scene >= 0 && s_scenes[scene].entry[k] && s_slot_owner[slot] != page) {
      /* Set back: a page whose content was not the boot's any more is the
       * game's state lost, unless the scene writes it before reading it. */
      if (memcmp(xl_mem + (page << 8), s_scene_init + (page << 8), 256) != 0) {
        size_t n = strlen(list);
        if (n < sizeof list - 8) snprintf(list + n, sizeof list - n, " %06X", page << 8);
        changed++;
      }
      memcpy(xl_mem + (page << 8), s_scene_init + (page << 8), 256);
      set++;
    }
    s_slot_owner[slot] = page;
  }
  printf("xl: scene %d from frame %u: %u pages set from the boot, %u of them not the boot's%s\n",
         scene, s_frame_count, set, changed, list);
  s_scene = scene;
}

static uint32_t current_function(void);

/* An access of the game's (xl_memmap_access) against the scene's pages. */
static void outside_check(uint32_t a, int size, int write) {
  if (!s_scenes_on || s_scene < 0) return;
  for (int k = 0; k < size; k++) {
    uint32_t b = (a + (uint32_t)k) & (XL_MEMORY - 1u), page = b >> 8;
    if (write) s_page_dirty[page] = 1;
    if (write == 2) continue; /* 2: the host's own loads, the files the game reads */
    uint32_t kind = 0;
    switch (s_page_kind[s_scene][page]) {
      case PAGE_FLASH:
        if (write) {
          kind = OUT_FLASH_WRITE;
        } else if (s_page_dirty[page]) {
          s_page_dirty[page] = 0;
          if (memcmp(xl_mem + (page << 8), s_scene_init + (page << 8), 256) != 0) kind = OUT_FLASH_STALE;
        }
        break;
      case PAGE_SPARE: kind = write ? OUT_SPARE_WRITE : OUT_SPARE_READ; break;
      case PAGE_SINK: kind = write ? 0 : OUT_SINK_READ; break;
      case PAGE_TEXT: kind = (b - 0xE00000u) % 128u >= 32u ? OUT_TEXT_PAST : 0; break;
      default: break;
    }
    if (kind == 0 || (s_outside_seen[s_scene][page] & kind)) continue;
    s_outside_seen[s_scene][page] |= (uint8_t)kind;
    uint32_t function = current_function();
    printf("xl: outside: scene %d frame %u %s at %06X (%s) in %06X\n", s_scene, s_frame_count,
           write ? "write" : "read", b,
           kind == OUT_FLASH_WRITE ? "flash"
           : kind == OUT_FLASH_STALE ? "flash, stale"
           : kind == OUT_SINK_READ ? "a sink"
           : kind == OUT_TEXT_PAST ? "text past 32" : "spare",
           function);
    if (s_outside_count < OUTSIDE_NOTES) {
      s_outside[s_outside_count].scene = (uint32_t)s_scene;
      s_outside[s_outside_count].frame = s_frame_count;
      s_outside[s_outside_count].address = b;
      s_outside[s_outside_count].kind = kind;
      s_outside[s_outside_count].function = function;
      s_outside_count++;
    }
  }
}

static void write_outside(const char *out_dir) {
  if (!s_scenes_on) return;
  char path[1024];
  snprintf(path, sizeof path, "%s/outside.txt", out_dir);
  FILE *f = fopen(path, "w");
  if (f == NULL) return;
  fprintf(f, "# scene frame address kind function (kind: 1 flash write, 2 spare read, "
             "4 spare write, 8 sink read, 16 text past 32, 32 stale flash read)\n");
  for (uint32_t k = 0; k < s_outside_count; k++) {
    fprintf(f, "%u %u %06X %u %06X\n", s_outside[k].scene, s_outside[k].frame,
            s_outside[k].address, s_outside[k].kind, s_outside[k].function);
  }
  fclose(f);
}

/* The functions on the stack when a phase starts: they run on in it, though
 * they were entered in the one before (the game's loop, entered once from
 * the title). Every long on the 68000 stack that could be a return address
 * into the code, by phase: stack_phases.txt at the end ("PHASE ADDRESS";
 * tools/xlat/rp_store.py keeps the ones that follow a call). */
#define STACK_NOTES 4096
static uint32_t s_stack_notes[STACK_NOTES][2];
static uint32_t s_stack_note_count;

static void note_stack(uint32_t phase) {
  for (uint32_t at = A(7); at + 4 <= A(7) + 0x10000u && at + 4 <= XL_MEMORY; at += 2) {
    uint32_t v = ((uint32_t)xl_mem[at] << 24) | ((uint32_t)xl_mem[at + 1] << 16) |
                 ((uint32_t)xl_mem[at + 2] << 8) | xl_mem[at + 3];
    if (v < XL_LOAD || v >= XL_BSS || (v & 1u)) continue;
    uint32_t k = 0;
    while (k < s_stack_note_count && !(s_stack_notes[k][0] == phase && s_stack_notes[k][1] == v)) k++;
    if (k == s_stack_note_count && k < STACK_NOTES) {
      s_stack_notes[k][0] = phase;
      s_stack_notes[k][1] = v;
      s_stack_note_count++;
    }
  }
}

static void write_stack_phases(const char *out_dir) {
  char path[1024];
  snprintf(path, sizeof path, "%s/stack_phases.txt", out_dir);
  FILE *f = fopen(path, "w");
  if (f == NULL) return;
  fprintf(f, "# phase address: a long on the stack when the phase started\n");
  for (uint32_t k = 0; k < s_stack_note_count; k++) {
    fprintf(f, "%u %06X\n", s_stack_notes[k][0], s_stack_notes[k][1]);
  }
  fclose(f);
}

void xl_trace_enter(uint32_t entry) {
  for (uint32_t n = 0; n < s_phase_count; n++) {
    if (s_phase_entries[n] == entry && s_phase != n + 1) {
#ifdef XL_MEMMAP
      memmap_phase_end(s_phase);
#endif
      s_phase = n + 1;
      note_stack(s_phase);
      uint32_t patterns = 0;
      for (uint32_t k = 0; k < sizeof s_phase_patterns; k++) patterns += s_phase_patterns[k];
      memset(s_phase_patterns, 0, sizeof s_phase_patterns);
      printf("xl: phase %u from frame %u, score %u (the phase before drew %u patterns)\n", s_phase,
             s_frame_count, rd32(XL_PLAYER_SCORE), patterns);
      if (s_scenes_on && s_scene_of_phase[s_phase] >= 0 && s_scene_of_phase[s_phase] != s_scene) {
        scene_enter(s_scene_of_phase[s_phase]);
      }
    }
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

/* The function entered last (the trace's), or 0 in a build without it. */
static uint32_t current_function(void) {
  for (uint32_t k = 1; k <= TRACE_RING; k++) {
    uint32_t *t = s_trace[(s_trace_next - k) % TRACE_RING];
    if (t[0] == 0 && t[1]) return t[1];
  }
  return 0;
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
/* Per page, in the current phase: 1 if a byte of it was read before the
 * phase wrote it (the page's value comes from before the phase). */
static uint8_t s_page_carried[XL_MEMORY >> 8];
static uint32_t s_block_reads[BLOCKS], s_block_writes[BLOCKS];
static uint32_t s_block_first[BLOCKS], s_block_last[BLOCKS];
static int s_memmap_paused;
/* The text plane over the whole run (not started over with the phases):
 * text_map.bin, a byte per address of its four planes with the flags above,
 * and text_scroll.txt, the scroll's values at the frames' ends. */
#define TEXT_MAP_BYTES (4u * 0x20000u)
static uint8_t s_text_map[TEXT_MAP_BYTES];
#define TEXT_SCROLLS 64u
static uint32_t s_text_scrolls[TEXT_SCROLLS][4]; /* x << 16 | y, frames, first, last */
static uint32_t s_text_scroll_count;

static void text_scroll_note(void) {
  uint32_t x = ((uint32_t)xl_mem[0xE80014u] << 8) | xl_mem[0xE80015u];
  uint32_t y = ((uint32_t)xl_mem[0xE80016u] << 8) | xl_mem[0xE80017u];
  uint32_t k = 0;
  while (k < s_text_scroll_count && s_text_scrolls[k][0] != (x << 16 | y)) k++;
  if (k == s_text_scroll_count) {
    if (k == TEXT_SCROLLS) return;
    s_text_scroll_count++;
    s_text_scrolls[k][0] = x << 16 | y;
    s_text_scrolls[k][2] = s_frame_count;
  }
  s_text_scrolls[k][1]++;
  s_text_scrolls[k][3] = s_frame_count;
}

void xl_memmap_access(uint32_t a, int size, int write) {
  if (s_memmap_paused) return;
  a &= 0xFFFFFFu;
#ifdef XL_TRACE
  outside_check(a, size, write);
#endif
  uint32_t block = a >> 8;
  if (write) {
    if (s_block_writes[block]++ == 0) s_block_first[block] = s_frame_count;
    s_block_last[block] = s_frame_count;
  } else {
    s_block_reads[block]++;
  }
  uint8_t flag = write == 0 ? 1u : write == 1 ? 2u : 4u;
  for (int k = 0; k < size; k++) {
    uint32_t b = (a + (uint32_t)k) & 0xFFFFFFu;
    if (write == 0 && !(s_touched[b] & 6u)) s_page_carried[b >> 8] = 1;
    s_touched[b] |= flag;
    if (b - 0xE00000u < TEXT_MAP_BYTES) s_text_map[b - 0xE00000u] |= flag;
  }
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
  snprintf(path, sizeof path, "%s/text_map.bin", out_dir);
  f = fopen(path, "wb");
  if (f) {
    fwrite(s_text_map, 1, TEXT_MAP_BYTES, f);
    fclose(f);
  }
  snprintf(path, sizeof path, "%s/text_scroll.txt", out_dir);
  f = fopen(path, "w");
  if (f) {
    fprintf(f, "# x y frames first-frame last-frame\n");
    for (uint32_t k = 0; k < s_text_scroll_count; k++) {
      fprintf(f, "%u %u %u %u %u\n", s_text_scrolls[k][0] >> 16, s_text_scrolls[k][0] & 0xFFFFu,
              s_text_scrolls[k][1], s_text_scrolls[k][2], s_text_scrolls[k][3]);
    }
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

/* With XL_PHASES too (a trace build): at each phase change and at the end,
 * what the phase touched in pages of 256 bytes (the RP's, xl.h's
 * XL_PAGE_BITS), to memmap_phases.txt, and the map starts over: per region
 * (main memory, the graphics pages, the text plane, the rest) the pages it
 * wrote (the code or the runtime for it) and the pages it only read. */
static FILE *s_phase_map;
static uint32_t s_phase_map_from;
/* The pages any stage wrote (phases from the first stage on): what RAM the
 * game's state needs across the stages, since it carries over. */
static uint8_t s_written_since_play[XL_MEMORY >> 8];

static void memmap_phase_end(uint32_t phase) {
  static const struct {
    uint32_t from, to;
    const char *name;
  } regions[] = {{0, 0xC00000u, "main"}, {0xC00000u, 0xE00000u, "gvram"},
                 {0xE00000u, 0xE80000u, "text"}, {0xE80000u, XL_MEMORY, "io"}};
  if (s_phase_map == NULL) {
    char path[1024];
    snprintf(path, sizeof path, "%s/memmap_phases.txt", s_out_dir);
    s_phase_map = fopen(path, "w");
    if (s_phase_map == NULL) return;
    fprintf(s_phase_map, "# phase from-frame to-frame, then per region: name written-pages "
                         "read-only-pages written-by-any-stage-so-far (256 bytes each)\n");
  }
  fprintf(s_phase_map, "%u %u %u", phase, s_phase_map_from, s_frame_count);
  for (size_t r = 0; r < sizeof regions / sizeof regions[0]; r++) {
    uint32_t written = 0, read = 0, since = 0;
    for (uint32_t page = regions[r].from; page < regions[r].to; page += 256u) {
      uint8_t flags = 0;
      for (uint32_t k = 0; k < 256u; k++) flags |= s_touched[page + k];
      if (flags & 6u) {
        written++;
        if (phase >= 2u) s_written_since_play[page >> 8] = 1; /* phase 2: the first stage */
      } else if (flags & 1u) {
        read++;
      }
      since += s_written_since_play[page >> 8];
    }
    fprintf(s_phase_map, " %s %u %u %u", regions[r].name, written, read, since);
  }
  fprintf(s_phase_map, "\n");
  fflush(s_phase_map);
  /* The phase's pages for a liveness pass (memmap_NN.bin, NN the phase's
   * order): per page, bit 0 written, bit 1 read before written. */
  static uint32_t sequence;
  char name[1024];
  snprintf(name, sizeof name, "%s/memmap_%02u.bin", s_out_dir, sequence++);
  FILE *f = fopen(name, "wb");
  if (f) {
    for (uint32_t page = 0; page < (XL_MEMORY >> 8); page++) {
      uint8_t flags = 0;
      for (uint32_t k = 0; k < 256u; k++) flags |= s_touched[(page << 8) + k];
      fputc(((flags & 6u) ? 1 : 0) | (s_page_carried[page] ? 2 : 0), f);
    }
    fclose(f);
  }
  memset(s_page_carried, 0, sizeof s_page_carried);
  memset(s_touched, 0, XL_MEMORY);
  s_phase_map_from = s_frame_count;
}
#else
#define memmap_range(a, n, write) ((void)0)
#define write_memmap(out_dir) ((void)0)
#define MEMMAP_PAUSE(on) ((void)0)
#define text_scroll_note() ((void)0)
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
/* Where a run resumed from a snapshot goes on: after the frame wait it was
 * taken at, XSP_VSYNC's (the stages) or WAIT_VBL's (the menus). */
#define XSP_VSYNC_RESUME 0x02021Au
#define WAIT_VBL_RESUME 0x022CB4u
static void write_snapshot(uint32_t resume);
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

static void keep_invincible(void);
static void save_replay(void);

/* The game's frame ends (either of its frame waits). */
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
  text_scroll_note();
  /* XL_MEMORY_AT=F: the memory at frame F's end, memory.bin (rp_store.py's
   * contents after the boot, for the pages a scene change sets). */
  static long memory_at = -2;
  if (memory_at == -2) memory_at = getenv("XL_MEMORY_AT") ? atol(getenv("XL_MEMORY_AT")) : -1;
  if (memory_at >= 0 && s_frame_count == (uint32_t)memory_at) {
    char path[1024];
    snprintf(path, sizeof path, "%s/memory.bin", s_out_dir);
    FILE *f = fopen(path, "wb");
    if (f) {
      fwrite(xl_mem, 1, XL_MEMORY, f);
      fclose(f);
    }
  }
  keep_invincible();
  save_replay();
  /* XL_SPRITE_FRAMES=A-B: the sprites drawn each frame from A to B (as the
   * port's measure.bin records them), to place a screen that differs. */
  static long sprite_from = -2, sprite_to;
  if (sprite_from == -2) {
    const char *v = getenv("XL_SPRITE_FRAMES");
    sprite_from = -1;
    if (v && sscanf(v, "%ld-%ld", &sprite_from, &sprite_to) != 2) sprite_from = -1;
  }
  if (sprite_from >= 0 && s_frame_count >= (uint32_t)sprite_from && s_frame_count <= (uint32_t)sprite_to) {
    printf("xl: frame %u: %u sprites\n", s_frame_count, s_last_sprite_count);
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
    write_snapshot(XSP_VSYNC_RESUME);
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
  if (s_snapshot_frame && s_frame_count >= s_snapshot_frame) {
    s_snapshot_frame = 0;
    write_snapshot(WAIT_VBL_RESUME);
  }
}

/* XSP_OUT: the game's sprite list (8 bytes a sprite) is the frame's; counted,
 * kept for the screen, and the list starts over. Returns 0. */
static uint8_t s_sprites[MAXIMUM_SPRITES * 8];
static uint32_t s_sprite_list_count;

/* Which patterns the sprite lists draw, and with which flips (bit n: the
 * attributes' bits 15-14 = n): pattern_use.bin at the end, 'CRSU' then a byte
 * a pattern (tools/xlat/rp_store.py converts those). */
#define PATTERNS 2048u
_Static_assert(sizeof s_phase_patterns == PATTERNS, "a pattern a byte");
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

/* XL_SPRITE_DUMP=A-B: each sprite list the game hands XSP_OUT in frames A
 * to B (the frames completed so far, as the port's frame_count), to
 * sprites.bin: the entries (u32), then each as the port's harness writes it
 * (port_build.py --dump-sprites): the frame (u32), the sprites (u32) and 512
 * slots of 8 bytes, big endian. Up to 16 lists. */
#define SPRITE_DUMP_ENTRIES 16u
#define SPRITE_DUMP_ENTRY (8u + 512u * 8u)
static void dump_sprite_list(uint32_t count) {
  static long from = -2, to;
  static uint8_t dump[SPRITE_DUMP_ENTRIES][SPRITE_DUMP_ENTRY];
  static uint32_t entries;
  if (from == -2) {
    const char *v = getenv("XL_SPRITE_DUMP");
    from = -1;
    if (v && sscanf(v, "%ld-%ld", &from, &to) != 2) from = -1;
  }
  if (from < 0 || s_frame_count < (uint32_t)from || s_frame_count > (uint32_t)to ||
      entries == SPRITE_DUMP_ENTRIES) {
    return;
  }
  uint8_t *e = dump[entries++];
  memset(e, 0, SPRITE_DUMP_ENTRY);
  for (int b = 0; b < 4; b++) {
    e[b] = (uint8_t)(s_frame_count >> (24 - 8 * b));
    e[4 + b] = (uint8_t)(count >> (24 - 8 * b));
  }
  memcpy(e + 8, xl_mem + XL_SPRITE_DATA_TABLE, count * 8u);
  char path[1024];
  snprintf(path, sizeof path, "%s/sprites.bin", s_out_dir);
  FILE *f = fopen(path, "wb");
  if (f == NULL) return;
  uint8_t n[4] = {(uint8_t)(entries >> 24), (uint8_t)(entries >> 16), (uint8_t)(entries >> 8),
                  (uint8_t)entries};
  fwrite(n, 1, 4, f);
  fwrite(dump, SPRITE_DUMP_ENTRY, entries, f);
  fclose(f);
}

void xl_patch_xsp_out(void) {
  uint32_t count = (rd32(XL_CURRENT_SPRITE_DATA_ENTRY) - XL_SPRITE_DATA_TABLE) / 8u;
  if (count > MAXIMUM_SPRITES) count = MAXIMUM_SPRITES;
  dump_sprite_list(count > 512u ? 512u : count);
  s_frame_sprite_count = count;
  s_sprite_list_count = count;
  memcpy(s_sprites, xl_mem + XL_SPRITE_DATA_TABLE, count * 8u);
  for (uint32_t k = 0; k < count; k++) {
    const uint8_t *e = s_sprites + 8u * k;
    uint32_t pattern = ((uint32_t)e[4] << 8) | e[5];
    if (pattern < PATTERNS) {
      s_pattern_use[pattern] |= (uint8_t)(1u << (e[6] >> 6));
      s_phase_patterns[pattern] = 1;
#ifdef XL_TRACE
      s_phase_use[s_phase][pattern] |= (uint8_t)(1u << (e[6] >> 6));
#endif
    }
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
 * (bytes of PCM for bytes of ADPCM). The effects' timing uses those lengths.
 * The decoded copies, signed 8-bit, laid out from DECODED_SAMPLES as the
 * table now points at them, go to effects.bin in the output folder: 'CRSE',
 * that base and the bytes (u32, little endian), 4 bytes of zero, then the
 * samples. The RP plays an _ADPCMOUT's address from there. */
#define SAMPLE_ENTRIES 256
#define SAMPLE_ENTRY_SIZE 16
#define SE_LENGTH 2
#define SE_ADDRESS 6
#define DECODED_SAMPLES 0x400000u
#define DECODED_MAX 0x100000u
#define EFFECTS_HEADER 16u
static int8_t s_decoded[DECODED_MAX];

/* The MSM6258's ADPCM, as MAME's okim6258 decodes it: a 12-bit signal
 * stepped by a table of 49 steps (16 x 1.1^n), the low nibble of each byte
 * first, from signal -2 and step 0 as each playback starts; the top 8 bits
 * kept. */
static void adpcm_decode(uint32_t from, uint32_t bytes, int8_t *out) {
  static const int k_shift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
  static int diff[49 * 16];
  if (diff[16] == 0) {
    for (int step = 0; step < 49; step++) {
      int v = (int)floor(16.0 * pow(1.1, step));
      for (int nib = 0; nib < 16; nib++) {
        int d = v / 8 + ((nib & 4) ? v : 0) + ((nib & 2) ? v / 2 : 0) + ((nib & 1) ? v / 4 : 0);
        diff[step * 16 + nib] = (nib & 8) ? -d : d;
      }
    }
  }
  int signal = -2, step = 0;
  for (uint32_t k = 0; k < 2u * bytes; k++) {
    int nib = (rd8(from + k / 2u) >> ((k & 1u) ? 4 : 0)) & 15;
    signal += diff[step * 16 + nib];
    if (signal > 2047) signal = 2047;
    if (signal < -2048) signal = -2048;
    step += k_shift[nib & 7];
    if (step < 0) step = 0;
    if (step > 48) step = 48;
    out[k] = (int8_t)(signal >> 4);
  }
}

static void write_effects(uint32_t bytes) {
  char path[1024];
  snprintf(path, sizeof path, "%s/effects.bin", s_out_dir);
  FILE *f = fopen(path, "wb");
  if (f == NULL) return;
  uint8_t head[EFFECTS_HEADER] = {'C', 'R', 'S', 'E'};
  for (int b = 0; b < 4; b++) {
    head[4 + b] = (uint8_t)(DECODED_SAMPLES >> (8 * b));
    head[8 + b] = (uint8_t)(bytes >> (8 * b));
  }
  fwrite(head, 1, sizeof head, f);
  fwrite(s_decoded, 1, bytes, f);
  fclose(f);
}

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
    if (next - DECODED_SAMPLES + length > DECODED_MAX) xl_fatal("the decoded samples overflow", 0);
    adpcm_decode(original[e], length / 2u, s_decoded + (next - DECODED_SAMPLES));
    wr32(at + SE_LENGTH, length);
    wr32(at + SE_ADDRESS, next);
    next += length;
  }
  s_samples_decoded = 1;
  if (s_out_dir) write_effects(next - DECODED_SAMPLES);
}

/* The host plays the demo as recorded, unless XL_LIVE_FROM names a frame:
 * from there a scripted player takes over (the stick left for a second,
 * right for a second, the shot held), to try the live path. */
static int s_live_read;

int xl_live_input(void) {
  static long from = -2;
  if (from == -2) {
    const char *v = getenv("XL_LIVE_FROM");
    from = v ? strtol(v, NULL, 10) : -1;
  }
  return from >= 0 && s_frame_count >= (uint32_t)from;
}

void xl_live_read(int on) { s_live_read = on; }

int xl_record(void) { return getenv("XL_SAVE_REPLAY_AT") != NULL; }

void xl_paused(void) {}

int xl_kakusi_key(void) { return 0; }

void xl_leave(uint32_t to) { (void)to; }

/* _JOYGET for a live read: active low, bit 2 left, bit 3 right, bit 5
 * (button A) the shot, held; bit 6 (button B) the bomb, a moment every six
 * seconds. */
static uint32_t scripted_stick(void) {
  uint32_t closed = ((s_frame_count / 55u) & 1u) ? 0x08u : 0x04u;
  closed |= 0x20u;
  if (s_frame_count % 330u < 3u) closed |= 0x40u;
  return 0xFFFFFF00u | (~closed & 0xFFu);
}

/* XL_INPUT names a script that drives every _JOYGET of stick 1, the menus'
 * as the game's: lines "FRAME MASK", the stick from that frame on, MASK the
 * bits held (hex: 1 up, 2 down, 4 left, 8 right, 20 button A, 40 button B;
 * the game reads them active low), or "auto" for the scripted player.
 * Lines in frame order; '#' starts a comment. */
#define INPUT_STEPS 256
#define INPUT_AUTO 0xFFFFFFFFu
static struct {
  uint32_t frame, mask;
} s_input[INPUT_STEPS];
static int s_input_steps = -1; /* -1: not read yet */

static int input_scripted(uint32_t *joyget) {
  if (s_input_steps < 0) {
    s_input_steps = 0;
    const char *path = getenv("XL_INPUT");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (path && !f) xl_fatal("XL_INPUT: no such script", 0);
    char line[128];
    while (f && fgets(line, sizeof line, f) && s_input_steps < INPUT_STEPS) {
      char *hash = strchr(line, '#');
      if (hash) *hash = 0;
      char word[32];
      unsigned frame;
      if (sscanf(line, "%u %31s", &frame, word) != 2) continue;
      s_input[s_input_steps].frame = frame;
      s_input[s_input_steps].mask =
          strcmp(word, "auto") == 0 ? INPUT_AUTO : (uint32_t)strtoul(word, NULL, 16) & 0xFFu;
      s_input_steps++;
    }
    if (f) fclose(f);
  }
  if (s_input_steps == 0) return 0;
  uint32_t mask = 0;
  for (int k = 0; k < s_input_steps && s_input[k].frame <= s_frame_count; k++) mask = s_input[k].mask;
  /* The game waits for the buttons to be released in loops with no frame
   * wait (the game over's first, $23C18): a frame that reads the stick more
   * than a frame's worth, the script lets go, as a player would (the port's
   * scripted build does the same: tools/re/port_build.py --scripted). */
  static uint32_t reads_frame = 0xFFFFFFFFu, reads;
  reads = s_frame_count == reads_frame ? reads + 1u : 0u;
  reads_frame = s_frame_count;
  *joyget = mask == INPUT_AUTO ? scripted_stick() : (0xFFFFFF00u | (~mask & 0xFFu));
  if (reads > 64u) *joyget |= 0x60u;
  return 1;
}

/* XL_INVINCIBLE: the hidden menu's invincibility (muteki) on from the boot,
 * set again whenever it reads 0 (the boot clears it once, before the main
 * menu): as a build of the port with `addq.w #1,INVINCIBILITY_MODE` in place
 * of that clear, so both replay a recording made with it the same way. */
static void keep_invincible(void) {
  static int on = -1;
  if (on < 0) on = getenv("XL_INVINCIBLE") != NULL;
  if (on && rd16(XL_INVINCIBILITY_MODE) == 0) wr16(XL_INVINCIBILITY_MODE, 1);
}

/* XL_SAVE_REPLAY_AT names a frame: there the game's recording so far goes
 * to replay.rep in the output folder, as SAVE_REPLAY_FILE writes it: the
 * header (14 bytes), the directions' length and bytes from the buffer's
 * start, the buttons' length and bytes up to its end (each length u32,
 * big endian, at most the buffer's 128 KB). */
#define REPLAY_BUFFER 0x20000u
static void save_replay(void) {
  static long at = -2;
  if (at == -2) {
    const char *v = getenv("XL_SAVE_REPLAY_AT");
    at = v ? strtol(v, NULL, 10) : -1;
  }
  if (at < 0 || s_frame_count != (uint32_t)at) return;
  uint32_t directions = rd32(XL_CURRENT_DEMO_RECORD_POINTER) - XL_DEMO_REPLAY_DATA + 1u;
  uint32_t buttons = XL_DEMO_REPLAY_DATA + REPLAY_BUFFER - rd32(XL_CURRENT_DEMO_RECORD_END_POINTER);
  if (directions > REPLAY_BUFFER) directions = REPLAY_BUFFER;
  if (buttons > REPLAY_BUFFER) buttons = REPLAY_BUFFER;
  char path[1024];
  snprintf(path, sizeof path, "%s/replay.rep", s_out_dir);
  FILE *f = fopen(path, "wb");
  if (f == NULL) xl_fatal("replay.rep: cannot write", 0);
  for (uint32_t k = 0; k < 14u; k++) fputc(rd8(XL_DEMO_REPLAY_HEADER + k), f);
  uint8_t be[4] = {(uint8_t)(directions >> 24), (uint8_t)(directions >> 16),
                   (uint8_t)(directions >> 8), (uint8_t)directions};
  fwrite(be, 1, 4, f);
  for (uint32_t k = 0; k < directions; k++) fputc(rd8(XL_DEMO_REPLAY_DATA + k), f);
  uint8_t be2[4] = {(uint8_t)(buttons >> 24), (uint8_t)(buttons >> 16), (uint8_t)(buttons >> 8),
                    (uint8_t)buttons};
  fwrite(be2, 1, 4, f);
  for (uint32_t k = 0; k < buttons; k++) fputc(rd8(XL_DEMO_REPLAY_DATA + REPLAY_BUFFER - buttons + k), f);
  fclose(f);
  printf("xl: replay.rep at frame %u: %u bytes of directions, %u of buttons\n", s_frame_count,
         directions, buttons);
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
    case 0x3B: { /* _JOYGET: nothing pressed (active low), the script, or the scripted player */
      uint32_t joyget;
      if ((D(1) & 0xFFu) == 0 && input_scripted(&joyget)) {
        D(0) = joyget;
        return;
      }
      D(0) = s_live_read ? scripted_stick() : 0xFFFFFFFFu;
      return;
    }
    case 0x60: { /* _ADPCMOUT: d1 = rate << 8 | channels, d2 = bytes, a1 = the sample */
      uint32_t channels = D(1) & 3u, length = D(2);
      if (s_effects_log) {
        fprintf(s_effects_log, "%u out %06X %u %04X\n", s_frame_count, A(1) & 0xFFFFFFu, length,
                D(1) & 0xFFFFu);
      }
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
      if (D(1) == 0 && s_effects_log) fprintf(s_effects_log, "%u stop\n", s_frame_count);
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

/* Where each IOCS call comes from: (function, the trap's address, the
 * return address on the stack then, calls), printed at the end with the
 * calls. */
#define CALL_SITES 64
static uint32_t s_call_sites[CALL_SITES][4];

static void note_call_site(uint32_t f, uint32_t pc) {
  uint32_t caller = rd32(A(7));
  for (int k = 0; k < CALL_SITES; k++) {
    uint32_t *c = s_call_sites[k];
    if (c[3] == 0 || (c[0] == f && c[1] == pc && c[2] == caller)) {
      c[0] = f;
      c[1] = pc;
      c[2] = caller;
      c[3]++;
      return;
    }
  }
}

/* MCDRV (trap #4), with XL_MUSIC set: the game finds a driver (as the
 * port's MCDRV 0.69 probe reports one, through the signature the original
 * looks for before the handler) and its calls are logged in music.txt:
 * the frame, the call, the song (the file its data was read from). */
#define MCDRV_VECTOR 0x90u
#define MCDRV_HANDLER 0x800u   /* in low memory nothing else uses */
#define MCDRV_VERSION 0x69u
#define MCDRV_TRANSMDC 0x01u
#define MCDRV_PLAYMUSIC 0x02u
#define MCDRV_STOPMUSIC 0x05u
#define MCDRV_FADEOUT 0x14u
static FILE *s_music_log;
static char s_music_song[96];
static char s_music_playing[96]; /* the song playing, and the frame it started */
static uint32_t s_music_since;

static void mcdrv_install(void) {
  wr32(MCDRV_HANDLER - 12u, 0x2D4D4344u); /* "-MCD" */
  wr32(MCDRV_HANDLER - 8u, 0x5256302Du);  /* "RV0-" */
  wr32(MCDRV_HANDLER - 4u, MCDRV_VERSION);
  wr32(MCDRV_VECTOR, MCDRV_HANDLER);
  char path[1024];
  snprintf(path, sizeof path, "%s/music.txt", s_out_dir);
  s_music_log = fopen(path, "w");
  if (s_music_log) fprintf(s_music_log, "# frame call song argument\n");
  snprintf(path, sizeof path, "%s/effects.txt", s_out_dir);
  s_effects_log = fopen(path, "w");
  if (s_effects_log) fprintf(s_effects_log, "# frame out address bytes mode | frame stop\n");
}

static void mcdrv(void) {
  uint32_t f = D(0);
  const char *what = f == MCDRV_TRANSMDC    ? "transfer"
                     : f == MCDRV_PLAYMUSIC ? "play"
                     : f == MCDRV_STOPMUSIC ? "stop"
                     : f == MCDRV_FADEOUT   ? "fade"
                                            : "other";
  if (f == MCDRV_TRANSMDC) {
    snprintf(s_music_song, sizeof s_music_song, "?%06X", A(0) & 0xFFFFFFu);
    for (uint32_t k = s_load_count; k-- > 0;) {
      if (s_loads[k].buffer == (A(0) & 0xFFFFFFu)) {
        snprintf(s_music_song, sizeof s_music_song, "%s", s_loads[k].name);
        break;
      }
    }
  }
  if (f == MCDRV_PLAYMUSIC) {
    memcpy(s_music_playing, s_music_song, sizeof s_music_playing);
    s_music_since = s_frame_count;
  } else if (f == MCDRV_STOPMUSIC || f == MCDRV_FADEOUT) {
    s_music_playing[0] = 0;
  }
  if (s_music_log) {
    fprintf(s_music_log, "%u %s %s %u", s_frame_count, what, s_music_song, D(1));
    if (strcmp(what, "other") == 0) fprintf(s_music_log, " (function %u)", f);
    fprintf(s_music_log, "\n");
  }
  D(0) = 0;
}

void xl_trap(uint32_t n, uint32_t pc) {
  if (n == 15) {
    note_call_site(D(0) & 0xFFu, pc);
    iocs();
  } else if (n == 4 && s_music_log) {
    mcdrv();
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

/* XL_FRESH_CARD: after the boot, as the RP's card before the game's first
 * save: an existing file opens as missing until this run has created it, so
 * a save takes the C library's create path (the RP's first save on a card
 * without SCORE.DAT). */
#define FRESH_FILES 8
static char s_created[FRESH_FILES][64];
static int s_created_count;

static int fresh_card_hides(const char *path) {
  static int fresh = -1;
  if (fresh < 0) fresh = getenv("XL_FRESH_CARD") != NULL;
  if (!fresh || s_frame_count <= 1u) return 0;
  for (int k = 0; k < s_created_count; k++) {
    if (strcmp(s_created[k], path + strlen(s_data_dir) + 1) == 0) return 0;
  }
  return 1;
}

static uint32_t open_file(uint32_t name, const char *mode) {
  int slot = 0;
  while (slot < FILE_HANDLES && s_files[slot] != NULL) slot++;
  if (slot == FILE_HANDLES) return (uint32_t)-4;
  char path[1024];
  host_path(path, sizeof path, name);
  if (mode[0] == 'r' && fresh_card_hides(path)) return (uint32_t)-2;
  if (mode[0] == 'w' && s_created_count < FRESH_FILES) {
    snprintf(s_created[s_created_count++], sizeof s_created[0], "%s", path + strlen(s_data_dir) + 1);
  }
  FILE *f = fopen(path, mode);
  if (f == NULL) return (uint32_t)-2;
  s_files[slot] = f;
  snprintf(s_file_names[slot], sizeof s_file_names[slot], "%s", path + strlen(s_data_dir) + 1);
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
#define DOS_LOG_LINES 20000u

void xl_linef(uint32_t word, uint32_t pc) {
  s_calls_linef[word & 0x1FFu]++;
  uint32_t args = A(7);
  uint32_t r = 0;
#ifdef XL_TRACE
  /* Every DOS call ("xl: dos" lines: what the RP would have to serve), a
   * file's name for the calls that take one. */
  /* Capped: a game that loops on a call (waiting for a key after a failed
   * save) would fill the disk. */
  static uint32_t s_dos_lines;
  if ((word & 0xFF00u) == 0xFF00u && ++s_dos_lines <= DOS_LOG_LINES) {
    if (s_dos_lines == DOS_LOG_LINES) printf("xl: dos: the log stops here\n");
    printf("xl: dos $%04X frame %u phase %u from %06X", word, s_frame_count, s_phase, pc);
    if (word == 0xFF3Cu || word == 0xFF3Du || word == 0xFF41u) {
      printf(" ");
      for (uint32_t p = rd32(args), k = 0; rd8(p) && k < 80u; p++, k++) putchar((int)rd8(p));
    }
    if (word == 0xFF3Fu || word == 0xFF40u) printf(" handle %u, %u bytes", rd16(args), rd32(args + 6));
    printf("\n");
  }
#endif
  /* XL_DISCARD_FILES: after the boot, the file calls as the RP answers them
   * until the game's files live on the card (bench_logic.c xl_linef): every
   * save succeeds and its bytes are dropped (_NAMECK served as usual). */
  static int discard = -1;
  if (discard < 0) discard = getenv("XL_DISCARD_FILES") != NULL;
  if (discard && s_frame_count > 1u &&
      (word == 0xFF36u || word == 0xFF3Cu || word == 0xFF3Du || word == 0xFF41u ||
       word == 0xFF3Eu || word == 0xFF3Fu || word == 0xFF40u || word == 0xFF42u)) {
    r = word == 0xFF36u                     ? 10000000u
        : word == 0xFF3Cu || word == 0xFF3Du ? 5u
        : word == 0xFF40u                    ? rd32(args + 6) & 0x7FFFFFFFu
                                             : 0u;
    D(0) = r;
    xl.n = (r >> 31) & 1u;
    xl.z = r == 0;
    xl.v = 0;
    xl.c = 0;
    return;
  }
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
#ifdef XL_TRACE
        printf("xl: read %s, %u bytes at %06X, frame %u (phase %u)\n", s_file_names[f - s_files], r,
               buffer & 0xFFFFFFu, s_frame_count, s_phase);
#endif
        if (s_load_count < LOADS) {
          s_loads[s_load_count].buffer = buffer & 0xFFFFFFu;
          s_loads[s_load_count].length = r;
          memcpy(s_loads[s_load_count].name, s_file_names[f - s_files], sizeof s_loads[0].name);
          s_load_count++;
        }
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
  /* The ST's colours stay as they are until the RP works them out again. */
  static uint16_t st[16];
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

/* Taken at the end of a frame wait (XSP_VSYNC's or WAIT_VBL's), so a resumed
 * run goes on at the instruction the patch continues at (xlat.py PATCHES), returning into the
 * callers through the return addresses on the 68000 stack. snapshot.bin:
 * 'XLSN', the resume address, the CPU (d0-d7, a0-a7, n z v c x, the system
 * byte), the frame and VBL counts, the sprite counts, the effect's end tick,
 * whether the samples are decoded, the sprite list's count, the checkpoints'
 * count (all u32 little-endian), the sprite list, the checkpoints so far,
 * then the 16 MB of memory. */
#define SNAPSHOT_WORDS 32

static void write_snapshot(uint32_t resume) {
  for (int k = 0; k < FILE_HANDLES; k++) {
    if (s_files[k]) xl_fatal("a snapshot with a file open", resume);
  }
  MEMMAP_PAUSE(1);
  uint32_t w[SNAPSHOT_WORDS], n = 0;
  w[n++] = 0x4E534C58u; /* 'XLSN' */
  w[n++] = resume;
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
  if (f == NULL) xl_fatal("cannot write snapshot.bin", resume);
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
  if (f == NULL) xl_fatal("cannot write resume.txt", resume);
  fprintf(f, "%06X # where the run resumes\n", resume);
  for (uint32_t at = A(7); at + 4 <= A(7) + 0x10000u && at + 4 <= XL_MEMORY; at += 2) {
    uint32_t v = rd32(at);
    if (v >= XL_LOAD && v < XL_BSS && !(v & 1u)) fprintf(f, "?%06X # at %06X\n", v, at);
  }
  fclose(f);

  /* With the music (XL_MUSIC): the songs the game has loaded, where (the
   * MCDRV calls name a song by its data's address), and the one playing. */
  if (s_music_log) {
    snprintf(path, sizeof path, "%s/songs.txt", s_out_dir);
    f = fopen(path, "w");
    if (f) {
      fprintf(f, "# address song (load); playing song since-frame\n");
      for (uint32_t k = 0; k < s_load_count; k++) {
        const char *n = s_loads[k].name;
        size_t len = strlen(n);
        if (len > 4 && (strcmp(n + len - 4, ".MDC") == 0 || strcmp(n + len - 4, ".mdc") == 0)) {
          fprintf(f, "load %06X %s\n", s_loads[k].buffer, n);
        }
      }
      if (s_music_playing[0]) fprintf(f, "playing %s %u\n", s_music_playing, s_music_since);
      fclose(f);
    }
  }
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
#ifdef XL_TRACE
  for (uint32_t phase = 0; phase <= PHASES; phase++) {
    uint32_t used = 0;
    for (uint32_t k = 0; k < PATTERNS; k++) used |= s_phase_use[phase][k];
    if (!used) continue;
    snprintf(path, sizeof path, "%s/pattern_use_p%02u.bin", s_out_dir, phase);
    f = fopen(path, "wb");
    if (f == NULL) return;
    fwrite("CRSU", 1, 4, f);
    fwrite(s_phase_use[phase], 1, PATTERNS, f);
    fclose(f);
  }
#endif
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

/* Stopped from outside (a run that hangs: the game waiting on something the
 * host does not serve): the coverage so far, so its last frames name the
 * loop. */
static void on_stop(int sig) {
  (void)sig;
#ifdef XL_TRACE
  if (s_out_dir) {
    write_coverage(s_out_dir);
    write_stack_phases(s_out_dir);
    write_outside(s_out_dir);
  }
#endif
  fprintf(stderr, "xl: stopped at frame %u\n", s_frame_count);
  _exit(3);
}

int main(int argc, char **argv) {
  signal(SIGTERM, on_stop);
  signal(SIGINT, on_stop);
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
  scenes_read();
#endif
  if (getenv("XL_MUSIC")) mcdrv_install();
  uint32_t start = XL_ENTRY;
  if (resume_path) start = read_snapshot(resume_path);
#ifdef XL_RP_RENDER
  /* The RP's renderer's list from the snapshot's, as the RP takes it. */
  if (resume_path) xl_render_take_sprites(&s_rp_frame, XL_SPRITE_DATA_TABLE, s_sprite_list_count);
#endif

  if (setjmp(s_exit) == 0) {
    uint32_t r = xl_call(start);
    for (;;) r = xl_call(r); /* code the program returned into */
  }
  write_checkpoints();
  write_pattern_use();
  if (s_music_log) fclose(s_music_log);
  if (s_effects_log) fclose(s_effects_log);
  MEMMAP_PAUSE(1);
  write_graphics();
  write_memmap(s_out_dir);
#if defined(XL_MEMMAP) && defined(XL_TRACE)
  if (s_phase_count) memmap_phase_end(s_phase); /* the last phase */
#endif
  for (uint32_t k = 0; k < 256; k++) {
    if (s_calls_iocs[k]) printf("xl: IOCS $%02X %u calls\n", k, s_calls_iocs[k]);
  }
  for (uint32_t k = 0; k < 512; k++) {
    if (s_calls_linef[k]) printf("xl: $%04X %u calls\n", 0xFE00u + k, s_calls_linef[k]);
  }
  for (int k = 0; k < CALL_SITES && s_call_sites[k][3]; k++) {
    printf("xl: IOCS $%02X from %06X (returning to %06X): %u calls\n", s_call_sites[k][0],
           s_call_sites[k][1], s_call_sites[k][2] & 0xFFFFFFu, s_call_sites[k][3]);
  }
#ifdef XL_TRACE
  write_coverage(s_out_dir);
  write_stack_phases(s_out_dir);
  write_outside(s_out_dir);
#endif
  printf("xl: %s after %u frames, %u checkpoints, %u VBLs\n",
         s_exit_reason == 1 ? "frame limit" : "the game exited", s_frame_count,
         s_checkpoint_count, s_total_vbl);
  return 0;
}
