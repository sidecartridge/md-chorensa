#include "install.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "chacha20.h"
#include "crc32.h"
#include "ikbd.h"
#include "install_members.h"
#include "palette.h"
#include "pico/time.h"
#include "resample_table.h"
#include "select.h"
#include "sha256.h"
#include "sound_convert.h"
#include "st_session.h"
#include "xl_store.h"
#include "zipread.h"

#define PATH_BYTES 128u
#define OUTPUTS (1u + 2u * INSTALL_SONGS) /* EFFECTS.SE8, then each song's .ST8 and .YM8 */
#define EFFECTS_FILE "EFFECTS.SE8"
#define EFFECTS_BASE 0x400000u /* where the game's sample table points (xl_host.c) */
#define DRAW_EVERY_US 500000u

extern const struct FB_FONT font8x8; /* fb.c */

/* Static: a FIL does not belong on core 0's stack. */
static FIL s_zip_file, s_out, s_out2, s_manifest, s_keyed; /* s_out2: a song's .YM8 beside its .ST8 */
static DIR s_dir;
static FILINFO s_info;

volatile install_stats_t installStats;

static const char *s_folder;
static void (*s_pump)(void);
static zip_t s_zip;
static void *s_work;

/* A song while its render inflates (song_work): its frames in mono kept in
 * a ring as they come, and both outputs made from it at once, each by one
 * resampler (its intro, then its loop). A loop starts with a few samples
 * whose filter reaches back into the loop's end, which comes last: they are
 * written as zeros, then made at the end from the loop's first and last
 * frames (head, tail) and written over the zeros. */
#define RING_FRAMES 2048u
#define HISTORY_FRAMES 128u
#define SONG_OUT_BYTES 8192u
typedef struct {
  sc_unblock_t unblock;
  uint8_t header[SC_CRM_HEADER];
  uint32_t header_bytes;
  int16_t ring[RING_FRAMES];     /* the frames, by number & (RING_FRAMES - 1) */
  uint32_t frames;               /* received so far */
  int16_t head[HISTORY_FRAMES];  /* the loop's first frames */
  int16_t tail[HISTORY_FRAMES];  /* its last */
  int want[2];                   /* [0] the .ST8, [1] the .YM8 */
  sc_song_t plan[2];
  sc_resampler_t r[2];
  int segment[2];                /* 0 the intro, 1 the loop, 2 done */
  uint32_t left[2];              /* samples of the segment still to make */
  uint32_t deferred[2];          /* the loop's first samples, made last */
  uint8_t out[2][SONG_OUT_BYTES];
  uint32_t out_bytes[2];
  uint32_t render_bytes;         /* the render's, in the zip: the bar */
  int failed;
} song_t;

_Static_assert(sizeof(song_t) <= INSTALL_SONG_WORK_BYTES, "INSTALL_SONG_WORK_BYTES too small");
_Static_assert(HISTORY_FRAMES >= RESAMPLE_YM_TAPS + 16u, "the history covers the deferred samples' taps");

/* --- Paths, the manifest ------------------------------------------------- */

static void path_of(char *out, const char *name) { snprintf(out, PATH_BYTES, "%s/%s", s_folder, name); }

static void output_name(uint32_t k, char *out) {
  if (k == 0) {
    strcpy(out, EFFECTS_FILE);
    return;
  }
  const char *path = k_install_songs[(k - 1u) / 2u].path;
  const char *name = strrchr(path, '/') + 1;
  size_t stem = strcspn(name, ".");
  snprintf(out, 16, "%.*s.%s", (int)stem, name, (k - 1u) % 2u ? "YM8" : "ST8");
}

static uint32_t file_size(const char *name) {
  char path[PATH_BYTES];
  path_of(path, name);
  return f_stat(path, &s_info) == FR_OK ? (uint32_t)s_info.fsize : 0;
}

/* The manifest's state: which outputs it lists as made (their sizes, for a
 * check that they are still there). */
static int read_manifest(uint32_t sizes[OUTPUTS]) {
  char path[PATH_BYTES], line[64], header[32];
  memset(sizes, 0, OUTPUTS * sizeof *sizes);
  path_of(path, INSTALL_MANIFEST);
  if (f_open(&s_manifest, path, FA_READ) != FR_OK) return 0;
  snprintf(header, sizeof header, "CHORENSA INSTALL %u\n", INSTALL_VERSION);
  int current = f_gets(line, sizeof line, &s_manifest) && strcmp(line, header) == 0;
  while (current && f_gets(line, sizeof line, &s_manifest)) {
    char name[16];
    unsigned long size;
    if (sscanf(line, "%15s %lu", name, &size) != 2) continue;
    for (uint32_t k = 0; k < OUTPUTS; k++) {
      char want[16];
      output_name(k, want);
      if (strcmp(name, want) == 0) sizes[k] = (uint32_t)size;
    }
  }
  f_close(&s_manifest);
  return current;
}

static int append_manifest(const char *line, int fresh) {
  char path[PATH_BYTES];
  path_of(path, INSTALL_MANIFEST);
  BYTE mode = FA_WRITE | (fresh ? FA_CREATE_ALWAYS : FA_OPEN_APPEND);
  if (f_open(&s_manifest, path, mode) != FR_OK) return INSTALL_CARD_ERROR;
  UINT wrote = 0;
  FRESULT r = f_write(&s_manifest, line, (UINT)strlen(line), &wrote);
  f_close(&s_manifest);
  return r == FR_OK && wrote == strlen(line) ? INSTALL_READY : INSTALL_CARD_ERROR;
}

/* --- The screen ------------------------------------------------------------ */

/* The screen is a few lines of text and the bar, drawn in bands as the
 * game's frames are (fb_draw_banded): only the chunky buffer's banded part
 * is used, its rest is the songs' buffers. 28 characters a line at most:
 * the frame is 256 pixels wide. */
#define SCREEN_LINES 10
#define TEXT_X 16
#define BAR_X 16
#define BAR_W 224
#define BAR_H 10
typedef struct {
  struct {
    int y;
    uint8_t colour;
    char text[29];
  } line[SCREEN_LINES];
  int lines;
  int bar_y;       /* -1: no bar */
  uint32_t bar;    /* its filled pixels */
} screen_t;
static screen_t s_screen;

static uint32_t s_started_us, s_drawn_us;
static uint32_t s_output; /* the output being made */
/* The bar measures the whole job in units, so it only moves forward: the
 * effects weigh their ADPCM bytes; each song three times its render's
 * bytes (its frames, its .ST8, its .YM8). What is already made counts as
 * done from the start. */
static uint64_t s_units_total, s_units_base, s_units_now, s_units_start, s_phase_units;

static void screen_start(void) {
  s_screen.lines = 0;
  s_screen.bar_y = -1;
}

static void say(int y, unsigned colour, const char *text) {
  if (s_screen.lines == SCREEN_LINES) return;
  s_screen.line[s_screen.lines].y = y;
  s_screen.line[s_screen.lines].colour = (uint8_t)colour;
  snprintf(s_screen.line[s_screen.lines].text, sizeof s_screen.line[0].text, "%s", text);
  s_screen.lines++;
}

/* The lines [y0, y1) of the screen at `band` (on either core). */
static void screen_band(uint8_t *band, const void *arg, int y0, int y1) {
  const screen_t *sc = arg;
  memset(band, 0, (size_t)(y1 - y0) * FB_CHUNKED_W);
  for (int k = 0; k < sc->lines; k++) {
    for (int row = 0; row < 8; row++) {
      int y = sc->line[k].y + row;
      if (y < y0 || y >= y1) continue;
      uint8_t *line = band + (size_t)(y - y0) * FB_CHUNKED_W;
      int x = TEXT_X;
      for (const char *c = sc->line[k].text; *c && x + 8 <= FB_CHUNKED_W; c++, x += 8) {
        int glyph = *c - font8x8.first_char;
        if (glyph < 0 || glyph >= font8x8.num_chars) continue;
        uint8_t bits = font8x8.data[glyph * font8x8.h + row];
        for (int b = 0; b < 8; b++)
          if (bits >> b & 1u) line[x + b] = sc->line[k].colour;
      }
    }
  }
  for (int y = sc->bar_y; sc->bar_y >= 0 && y < sc->bar_y + BAR_H; y++) {
    if (y < y0 || y >= y1) continue;
    uint8_t *line = band + (size_t)(y - y0) * FB_CHUNKED_W + BAR_X;
    memset(line, 7, BAR_W);
    memset(line, 10, sc->bar);
  }
}

static void screen_show(void) {
  fb_draw_banded(screen_band, &s_screen);
  fb_republish();
}

static void draw(int force) {
  uint32_t now = time_us_32();
  /* An ST that started again shows nothing until a new frame: drawn now. */
  if (st_session_consume_boot()) force = 1;
  if (!force && now - s_drawn_us < DRAW_EVERY_US) return;
  s_drawn_us = now;
  static const uint16_t k_colours[PALETTE_ENTRIES] = {[0] = 0x000, [7] = 0x555, [10] = 0x070,
                                                      [14] = 0x770, [15] = 0x777};
  palette_set(k_colours);
  screen_start();
  char line[40], name[16];
  say(48, 14, "PREPARING THE GAME");
  say(64, 15, "FROM THE FILES IN THE CARD'S");
  snprintf(line, sizeof line, "%s FOLDER.", s_folder);
  say(76, 15, line);
  if (s_output >= OUTPUTS) {
    say(108, 15, "THE GAME'S IMAGES");
  } else {
    output_name(s_output, name);
    snprintf(line, sizeof line, "SOUND %lu OF %u: %s", (unsigned long)(s_output + 1u), OUTPUTS, name);
    say(108, 15, line);
  }
  s_screen.bar_y = 124;
  s_screen.bar = s_units_total ? (uint32_t)(BAR_W * s_units_now / s_units_total) : 0;
  installStats.bar = s_screen.bar;
  uint32_t seconds = (now - s_started_us) / 1000000u;
  snprintf(line, sizeof line, "%lu:%02lu ELAPSED", (unsigned long)(seconds / 60u),
           (unsigned long)(seconds % 60u));
  say(144, 15, line);
  /* The time left, from this run's pace once it has one. */
  uint64_t done = s_units_now - s_units_start;
  if (seconds < 15u || done == 0) {
    snprintf(line, sizeof line, "WORKING OUT THE TIME LEFT");
  } else {
    uint32_t left = (uint32_t)((uint64_t)seconds * (s_units_total - s_units_now) / done);
    if (left < 60u) snprintf(line, sizeof line, "UNDER A MINUTE LEFT");
    else snprintf(line, sizeof line, "ABOUT %lu MIN LEFT", (unsigned long)((left + 59u) / 60u));
  }
  say(156, 15, line);
  say(188, 7, "THIS HAPPENS ONCE.");
  screen_show();
}

/* A piece of work weighing `units` begins; `part` of `parts` of it done;
 * it ends. */
static void work_begin(uint64_t units) {
  s_phase_units = units;
  s_units_now = s_units_base;
}
static void progress(uint64_t part, uint64_t parts) {
  s_units_now = s_units_base + (parts ? (part < parts ? part : parts) * s_phase_units / parts : 0);
  s_pump();
  draw(0);
}
static void work_end(void) {
  s_units_base += s_phase_units;
  s_units_now = s_units_base;
}

/* --- The zip --------------------------------------------------------------- */

static int32_t zip_read(void *ctx, uint32_t offset, void *buffer, uint32_t length) {
  (void)ctx;
  UINT got = 0;
  if (f_lseek(&s_zip_file, offset) != FR_OK) return -1;
  if (f_read(&s_zip_file, buffer, length, &got) != FR_OK) return -1;
  return (int32_t)got;
}

/* The first zip in the folder that holds the repository's program. */
static int open_zip(void) {
  FRESULT r = f_findfirst(&s_dir, &s_info, s_folder, "*.zip");
  while (r == FR_OK && s_info.fname[0]) {
    char path[PATH_BYTES];
    path_of(path, s_info.fname);
    if (f_open(&s_zip_file, path, FA_READ) == FR_OK) {
      zip_member_t m;
      if (zip_open(&s_zip, zip_read, NULL, (uint32_t)f_size(&s_zip_file)) == ZIP_OK &&
          zip_find(&s_zip, INSTALL_PROGRAM_PATH, &m) == ZIP_OK) {
        f_closedir(&s_dir);
        DPRINTF("install: %s\n", path);
        return INSTALL_READY;
      }
      f_close(&s_zip_file);
    }
    r = f_findnext(&s_dir, &s_info);
  }
  f_closedir(&s_dir);
  return INSTALL_NO_ZIP;
}

/* The member, checked against the tested one. */
static int member(const install_member_t *want, zip_member_t *m) {
  if (zip_find(&s_zip, want->path, m) != ZIP_OK) return INSTALL_BAD_MEMBER;
  if (m->size != want->size || m->crc != want->crc) return INSTALL_BAD_MEMBER;
  return INSTALL_READY;
}

/* A member's extraction: a read that failed or came back damaged (bad
 * deflate data, a CRC that differs from the directory's) is INSTALL_RETRY,
 * read again by the caller up to INSTALL_TRIES times. */
#define INSTALL_RETRY (-100)
static int extract_status(int z) {
  if (z == ZIP_OK) return INSTALL_READY;
  installStats.error = z;
  if (z == ZIP_ERR_STOPPED) return INSTALL_CARD_ERROR;
  if (z == ZIP_ERR_READ || z == ZIP_ERR_DATA || z == ZIP_ERR_CHECK) return INSTALL_RETRY;
  return INSTALL_BAD_MEMBER;
}

static int write_all(FIL *f, const void *data, uint32_t length) {
  UINT wrote = 0;
  return f_write(f, data, length, &wrote) == FR_OK && wrote == length ? 0 : -1;
}

static int create(FIL *f, const char *name) {
  char path[PATH_BYTES];
  path_of(path, name);
  return f_open(f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK ? 0 : -1;
}

/* --- The effects ------------------------------------------------------------ */

typedef struct {
  sc_adpcm_t adpcm;
  uint32_t done, total;
} effects_t;

static int effects_piece(void *arg, const uint8_t *data, uint32_t length) {
  effects_t *e = arg;
  int8_t out[1024];
  while (length) {
    uint32_t n = length < sizeof out / 2u ? length : sizeof out / 2u;
    sc_adpcm_decode(&e->adpcm, data, n, out);
    if (write_all(&s_out, out, 2u * n)) return 1;
    installStats.inflated += n;
    installStats.written += 2u * n;
    data += n;
    length -= n;
    e->done += n;
  }
  progress(e->done, e->total);
  return 0;
}

static int make_effects(void) {
  effects_t e = {0};
  for (uint32_t k = 0; k < INSTALL_EFFECTS; k++) e.total += k_install_effects[k].size;
  uint8_t header[16] = {'C', 'R', 'S', 'E'};
  for (int b = 0; b < 4; b++) {
    header[4 + b] = (uint8_t)(EFFECTS_BASE >> (8 * b));
    header[8 + b] = (uint8_t)((2u * e.total) >> (8 * b));
  }
  if (create(&s_out, EFFECTS_FILE) || write_all(&s_out, header, sizeof header)) {
    f_close(&s_out);
    return INSTALL_CARD_ERROR;
  }
  int status = INSTALL_READY;
  for (uint32_t k = 0; k < INSTALL_EFFECTS && status == INSTALL_READY; k++) {
    zip_member_t m;
    status = member(&k_install_effects[k], &m);
    if (status != INSTALL_READY) break;
    FSIZE_t at = f_tell(&s_out);
    uint32_t done = e.done;
    for (int tries = 0;; tries++) {
      sc_adpcm_start(&e.adpcm);
      status = extract_status(zip_extract(&s_zip, &m, s_work, effects_piece, &e));
      if (status != INSTALL_RETRY) break;
      if (tries + 1 == INSTALL_TRIES) {
        status = INSTALL_READ_ERROR;
        break;
      }
      installStats.retries++;
      e.done = done;
      if (f_lseek(&s_out, at) != FR_OK) {
        status = INSTALL_CARD_ERROR;
        break;
      }
    }
  }
  if (f_close(&s_out) != FR_OK && status == INSTALL_READY) status = INSTALL_CARD_ERROR;
  return status;
}

/* --- The images ------------------------------------------------------------ */

#define IMAGES 10u
static const char *const k_images[IMAGES] = {"COMMON.IMG", "MENUS.IMG",  "STAGE1.IMG",
                                             "STAGE2.IMG", "STAGE3.IMG", "STAGE4.IMG",
                                             "STAGE5.IMG", "STAGE6.IMG", "STAGE7.IMG",
                                             XL_STORE_START_FILE};
#define KEYED_HEADER 64u
#define KEYED_VERSION 1u
#define CHUNK_BYTES (16u * 1024u)

typedef struct {
  uint32_t version, size, crc;
  char name[17];
  char firmware[XL_STORE_FIRMWARE_ID + 1];
} keyed_t;

static void keyed_name(const char *name, char *out) {
  snprintf(out, 16, "%s", name);
  out[strlen(out) - 1u] = 'K'; /* COMMON.IMG: COMMON.IMK; START.STA: START.STK */
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int same_firmware(const char *a, const char *firmware) {
  return strncmp(a, firmware, XL_STORE_FIRMWARE_ID - 1u) == 0;
}

/* Image k on the card, made for this firmware (its header says so). */
static int image_current(uint32_t k, const char *firmware) {
  char path[PATH_BYTES];
  uint8_t h[64];
  UINT got = 0;
  path_of(path, k_images[k]);
  if (f_open(&s_keyed, path, FA_READ) != FR_OK) return 0;
  int ok = f_read(&s_keyed, h, sizeof h, &got) == FR_OK && got == sizeof h;
  f_close(&s_keyed);
  if (!ok) return 0;
  int start = k == IMAGES - 1u;
  uint32_t magic = le32(h), at = start ? offsetof(xl_store_start_t, firmware)
                                       : offsetof(xl_store_header_t, firmware);
  char fw[XL_STORE_FIRMWARE_ID + 1] = {0};
  memcpy(fw, h + at, XL_STORE_FIRMWARE_ID);
  return magic == (start ? XL_STORE_START_MAGIC : XL_STORE_MAGIC) && same_firmware(fw, firmware);
}

/* Image k's keyed copy: its header, checked; the file left open at its data. */
static int open_keyed(uint32_t k, const char *firmware, keyed_t *t) {
  char name[16], path[PATH_BYTES];
  uint8_t h[KEYED_HEADER];
  UINT got = 0;
  keyed_name(k_images[k], name);
  path_of(path, name);
  if (f_open(&s_keyed, path, FA_READ) != FR_OK) return INSTALL_NO_PACK;
  if (f_read(&s_keyed, h, sizeof h, &got) != FR_OK || got != sizeof h || memcmp(h, "CRSK", 4) != 0) {
    f_close(&s_keyed);
    return INSTALL_BAD_PACK;
  }
  t->version = le32(h + 4);
  t->size = le32(h + 8);
  t->crc = le32(h + 12);
  memcpy(t->name, h + 16, 16);
  t->name[16] = '\0';
  memcpy(t->firmware, h + 32, XL_STORE_FIRMWARE_ID);
  t->firmware[XL_STORE_FIRMWARE_ID] = '\0';
  if (t->version != KEYED_VERSION || strcmp(t->name, k_images[k]) != 0) {
    f_close(&s_keyed);
    return INSTALL_BAD_PACK;
  }
  if (!same_firmware(t->firmware, firmware)) {
    f_close(&s_keyed);
    return INSTALL_OLD_PACK;
  }
  return INSTALL_READY;
}

/* The key: SHA-256 of the zip's program, as it inflates. */
static int hash_piece(void *arg, const uint8_t *data, uint32_t length) {
  sha256_update(arg, data, length);
  installStats.inflated += length;
  progress(installStats.inflated, k_install_program.size);
  return 0;
}

static int program_key(uint8_t key[32]) {
  zip_member_t m;
  int status = member(&k_install_program, &m);
  for (int tries = 0; status == INSTALL_READY; tries++) {
    sha256_t s;
    sha256_start(&s);
    installStats.inflated = 0;
    status = extract_status(zip_extract(&s_zip, &m, s_work, hash_piece, &s));
    if (status == INSTALL_READY) sha256_end(&s, key);
    if (status != INSTALL_RETRY) break;
    if (tries + 1 == INSTALL_TRIES) return INSTALL_READ_ERROR;
    installStats.retries++;
    status = INSTALL_READY;
  }
  return status;
}

/* Image k unlocked from its keyed copy (open at its data) onto the card. */
static int unlock(uint32_t k, const keyed_t *t, const uint8_t key[32]) {
  uint8_t nonce_hash[32];
  sha256_t s;
  sha256_start(&s);
  sha256_update(&s, k_images[k], (uint32_t)strlen(k_images[k]) + 1u); /* the name and its 0 */
  sha256_update(&s, t->firmware, (uint32_t)strlen(t->firmware));
  sha256_end(&s, nonce_hash);
  chacha20_t c;
  chacha20_start(&c, key, nonce_hash);
  int status = create(&s_out, k_images[k]) ? INSTALL_CARD_ERROR : INSTALL_READY;
  uint8_t *chunk = s_work;
  uint32_t done = 0, crc = CRC32_START;
  while (status == INSTALL_READY && done < t->size) {
    UINT got = 0;
    uint32_t n = t->size - done < CHUNK_BYTES ? t->size - done : CHUNK_BYTES;
    if (f_read(&s_keyed, chunk, n, &got) != FR_OK || got != n) {
      status = INSTALL_BAD_PACK;
      break;
    }
    chacha20_xor(&c, chunk, n);
    crc = crc32_update(crc, chunk, n);
    if (write_all(&s_out, chunk, n)) status = INSTALL_CARD_ERROR;
    done += n;
    installStats.written += n;
    progress(done, t->size);
  }
  if (f_close(&s_out) != FR_OK && status == INSTALL_READY) status = INSTALL_CARD_ERROR;
  f_close(&s_keyed);
  if (status == INSTALL_READY && crc32_end(crc) != t->crc) status = INSTALL_BAD_PACK;
  if (status != INSTALL_READY) {
    char path[PATH_BYTES];
    path_of(path, k_images[k]);
    f_unlink(path); /* no half image left for the store */
  }
  return status;
}

/* --- The songs ---------------------------------------------------------------- */

static int emit(song_t *g, int o, uint8_t sample) {
  g->out[o][g->out_bytes[o]++] = sample;
  if (g->out_bytes[o] < SONG_OUT_BYTES) return 0;
  installStats.written += g->out_bytes[o];
  int r = write_all(o ? &s_out2 : &s_out, g->out[o], g->out_bytes[o]);
  g->out_bytes[o] = 0;
  return r;
}

/* The stream's frame `index` from the ring (the resamplers ask only for
 * frames received and still there). */
static int ring_frame(void *ctx, uint32_t index) {
  song_t *g = ctx;
  int32_t f = sc_song_source(&g->unblock.crm, index);
  return f < 0 ? 0 : g->ring[(uint32_t)f & (RING_FRAMES - 1u)];
}

/* The same from the loop's first and last frames, for the deferred samples. */
static int history_frame(void *ctx, uint32_t index) {
  song_t *g = ctx;
  const sc_crm_t *c = &g->unblock.crm;
  int32_t f = sc_song_source(c, index);
  if (f < 0) return 0;
  uint32_t tail_from = c->intro + c->loop - HISTORY_FRAMES;
  if ((uint32_t)f >= tail_from) return g->tail[(uint32_t)f - tail_from];
  if ((uint32_t)f >= c->intro && (uint32_t)f < c->intro + HISTORY_FRAMES)
    return g->head[(uint32_t)f - c->intro];
  g->failed = INSTALL_BAD_MEMBER;
  return 0;
}

static uint32_t taps_lead(uint32_t rate) {
  return (rate == RESAMPLE_ST_RATE ? RESAMPLE_ST_TAPS : RESAMPLE_YM_TAPS) / 2u - 1u;
}

/* Output o's loop, after its intro: its first samples whose filter reaches
 * back before the loop's third pass (into the second's end) are deferred
 * (zeros now), the rest made as the frames come. */
static int start_loop(song_t *g, int o) {
  const sc_crm_t *c = &g->unblock.crm;
  const sc_song_t *p = &g->plan[o];
  g->segment[o] = 2;
  if (p->loop == 0) return 0;
  uint32_t lead = taps_lead(p->rate), d = 0;
  while ((int64_t)((uint64_t)(p->loop_from + d) * 22050u / p->rate) - (int64_t)lead <
         (int64_t)c->intro + 2 * (int64_t)c->loop)
    d++;
  if (d > p->loop || c->loop < 2u * HISTORY_FRAMES) return INSTALL_BAD_MEMBER;
  for (uint32_t k = 0; k < d; k++)
    if (emit(g, o, 0)) return INSTALL_CARD_ERROR;
  g->deferred[o] = d;
  sc_resampler_start(&g->r[o], p->rate, p->loop_from + d, ring_frame, g);
  g->left[o] = p->loop - d;
  g->segment[o] = 1;
  return 0;
}

/* Each output as far as the frames received allow (all, at the end). */
static int advance(song_t *g) {
  const sc_crm_t *c = &g->unblock.crm;
  uint32_t total = c->intro + c->loop;
  for (int o = 0; o < 2; o++) {
    if (!g->want[o]) continue;
    int32_t lead = (int32_t)taps_lead(g->plan[o].rate), taps = (int32_t)g->r[o].taps;
    while (g->segment[o] < 2) {
      if (g->left[o] == 0) {
        int r = g->segment[o] == 0 ? start_loop(g, o) : (g->segment[o] = 2, 0);
        if (r) return r;
        continue;
      }
      int32_t last = sc_song_source(c, g->r[o].at + (uint32_t)(taps - lead - 1));
      if (last >= 0 ? (uint32_t)last >= g->frames : g->frames < total) break;
      if (emit(g, o, (uint8_t)sc_resampler_next(&g->r[o]))) return INSTALL_CARD_ERROR;
      g->left[o]--;
    }
  }
  return 0;
}

static int mono_piece(void *arg, const int16_t *mono, uint32_t frames) {
  song_t *g = arg;
  const sc_crm_t *c = &g->unblock.crm;
  for (uint32_t k = 0; k < frames; k++, g->frames++) {
    uint32_t f = g->frames;
    g->ring[f & (RING_FRAMES - 1u)] = mono[k];
    if (f >= c->intro && f < c->intro + HISTORY_FRAMES) g->head[f - c->intro] = mono[k];
    if (c->loop >= HISTORY_FRAMES && f >= c->intro + c->loop - HISTORY_FRAMES)
      g->tail[f - (c->intro + c->loop - HISTORY_FRAMES)] = mono[k];
  }
  int r = advance(g);
  if (r) g->failed = r;
  return r;
}

/* The render's bytes: its header (the outputs then planned and begun),
 * then its body, unblocked to mono frames. */
static int crm_piece(void *arg, const uint8_t *data, uint32_t length) {
  static const uint32_t k_rates[2] = {RESAMPLE_ST_RATE, RESAMPLE_YM_RATE};
  song_t *g = arg;
  installStats.inflated += length;
  if (g->header_bytes < SC_CRM_HEADER) {
    uint32_t n = SC_CRM_HEADER - g->header_bytes < length ? SC_CRM_HEADER - g->header_bytes : length;
    memcpy(g->header + g->header_bytes, data, n);
    g->header_bytes += n;
    data += n;
    length -= n;
    if (g->header_bytes == SC_CRM_HEADER) {
      sc_crm_t crm;
      if (sc_crm_header(g->header, &crm)) {
        g->failed = INSTALL_BAD_MEMBER;
        return 1;
      }
      sc_unblock_start(&g->unblock, &crm);
      for (int o = 0; o < 2; o++) {
        if (!g->want[o]) continue;
        uint8_t header[SC_SONG_HEADER];
        if (sc_song_plan(&crm, k_rates[o], &g->plan[o])) {
          g->failed = INSTALL_BAD_MEMBER;
          return 1;
        }
        sc_song_header(&crm, &g->plan[o], header);
        if (write_all(o ? &s_out2 : &s_out, header, sizeof header)) {
          g->failed = INSTALL_CARD_ERROR;
          return 1;
        }
        sc_resampler_start(&g->r[o], k_rates[o], 0, ring_frame, g);
        g->left[o] = g->plan[o].intro;
        g->segment[o] = 0;
      }
    }
  }
  if (length && sc_unblock(&g->unblock, data, length, mono_piece, g)) {
    if (!g->failed) g->failed = INSTALL_CARD_ERROR;
    return 1;
  }
  progress(installStats.inflated, g->render_bytes);
  return 0;
}

/* Song `index`'s outputs still to make (want[0] its .ST8, want[1] its
 * .YM8), in one pass over its render as it inflates. */
static int make_song(song_t *g, uint32_t index, const int want[2], const char *const names[2]) {
  zip_member_t m;
  int status = member(&k_install_songs[index], &m);
  if (status != INSTALL_READY) return status;
  memset(g, 0, sizeof *g);
  g->want[0] = want[0];
  g->want[1] = want[1];
  g->render_bytes = m.size;
  for (int o = 0; o < 2; o++) {
    g->segment[o] = 2;
    if (want[o] && create(o ? &s_out2 : &s_out, names[o])) status = INSTALL_CARD_ERROR;
  }
  if (status == INSTALL_READY) status = extract_status(zip_extract(&s_zip, &m, s_work, crm_piece, g));
  if (g->failed) status = g->failed;
  if (status == INSTALL_READY && g->frames != g->unblock.crm.intro + g->unblock.crm.loop)
    status = INSTALL_BAD_MEMBER;
  if (status == INSTALL_READY) status = advance(g); /* past the end: silence */
  for (int o = 0; o < 2; o++) {
    if (!want[o]) continue;
    FIL *f = o ? &s_out2 : &s_out;
    if (status == INSTALL_READY && g->out_bytes[o]) {
      installStats.written += g->out_bytes[o];
      if (write_all(f, g->out[o], g->out_bytes[o])) status = INSTALL_CARD_ERROR;
    }
    /* The loop's deferred samples, over their zeros. */
    if (status == INSTALL_READY && g->deferred[o]) {
      uint8_t first[64];
      sc_resampler_t r;
      if (g->deferred[o] > sizeof first) status = INSTALL_BAD_MEMBER;
      if (status == INSTALL_READY)
        sc_resampler_start(&r, g->plan[o].rate, g->plan[o].loop_from, history_frame, g);
      for (uint32_t k = 0; status == INSTALL_READY && k < g->deferred[o]; k++)
        first[k] = (uint8_t)sc_resampler_next(&r);
      if (g->failed) status = g->failed;
      if (status == INSTALL_READY &&
          (f_lseek(f, SC_SONG_HEADER + g->plan[o].intro) != FR_OK ||
           write_all(f, first, g->deferred[o])))
        status = INSTALL_CARD_ERROR;
    }
    if (f_close(f) != FR_OK && status == INSTALL_READY) status = INSTALL_CARD_ERROR;
  }
  return status;
}

/* --- The gate ------------------------------------------------------------------ */

/* The phase now `p` (0: between outputs): the last one's time kept. */
static uint32_t s_phase_us;
static void phase(uint32_t p) {
  uint32_t now = time_us_32();
  if (installStats.phase) installStats.phase_ms[installStats.phase] = (now - s_phase_us) / 1000u;
  installStats.phase = p;
  installStats.inflated = 0;
  installStats.written = 0;
  s_phase_us = now;
}

/* Whether output k is one the sound path uses: the effects, and each
 * song's .ST8 (the DMA chip) or .YM8 (the YM). */
static int needed(uint32_t k, int path) {
  return k == 0 || (int)((k - 1u) % 2u) == path;
}

int install_gate(const char *folder, void *work, void *song_work, int path, const char *firmware,
                 void (*pump)(void)) {
  s_folder = folder;
  s_work = work;
  s_pump = pump;
  installStats.error = 0;
  uint32_t sizes[OUTPUTS];
  int current = read_manifest(sizes);
  /* Made and still there, at the size made; those the path needs counted. */
  uint32_t missing = 0;
  for (uint32_t k = 0; k < OUTPUTS; k++) {
    char name[16];
    output_name(k, name);
    if (sizes[k] == 0 || file_size(name) != sizes[k]) {
      sizes[k] = 0;
      if (needed(k, path)) missing++;
    }
  }
  /* The images current for this firmware, or each one's keyed copy. */
  uint32_t lock = 0; /* the images to unlock, one bit each */
  uint64_t image_units = 0;
  for (uint32_t k = 0; k < IMAGES; k++) {
    if (image_current(k, firmware)) continue;
    keyed_t t;
    int r = open_keyed(k, firmware, &t);
    if (r != INSTALL_READY) return r;
    f_close(&s_keyed);
    lock |= 1u << k;
    image_units += t.size;
  }
  if (current && missing == 0 && lock == 0) return INSTALL_READY;
  DPRINTF("install: %lu of %u files to make\n", (unsigned long)missing, OUTPUTS);

  int status = open_zip();
  if (status != INSTALL_READY) return status;
  char line[64];
  if (!current) {
    snprintf(line, sizeof line, "CHORENSA INSTALL %u\n", INSTALL_VERSION);
    status = append_manifest(line, 1);
  }
  song_t *b = song_work;
  /* The job's units, and those already made. */
  uint64_t effects_units = 0;
  for (uint32_t k = 0; k < INSTALL_EFFECTS; k++) effects_units += k_install_effects[k].size;
  s_units_total = effects_units;
  s_units_base = sizes[0] ? effects_units : 0;
  /* A song the path needs weighs twice its render (its frames, its
   * output); one already made counts as done. */
  for (uint32_t i = 0; i < INSTALL_SONGS; i++) {
    uint64_t render = k_install_songs[i].size;
    s_units_total += 2u * render;
    if (sizes[1u + 2u * i + (uint32_t)path]) s_units_base += 2u * render;
  }
  if (lock) s_units_total += image_units + k_install_program.size;
  s_units_now = s_units_start = s_units_base;
  s_started_us = time_us_32();
  s_drawn_us = 0;
  /* The images first: the game needs them as much as its sound. */
  if (lock) {
    uint8_t key[32];
    s_output = OUTPUTS; /* the screen names the images */
    draw(1);
    phase(5);
    work_begin(k_install_program.size);
    status = program_key(key);
    work_end();
    for (uint32_t k = 0; k < IMAGES && status == INSTALL_READY; k++) {
      if (!(lock >> k & 1u)) continue;
      keyed_t t;
      status = open_keyed(k, firmware, &t);
      if (status != INSTALL_READY) break;
      work_begin(t.size);
      status = unlock(k, &t, key);
      work_end();
    }
    memset(key, 0, sizeof key);
    phase(0);
  }
  if (!sizes[0]) {
    s_output = 0;
    draw(1);
    uint32_t t0 = time_us_32();
    installStats.output = 0;
    phase(4);
    work_begin(effects_units);
    status = make_effects();
    work_end();
    installStats.output_ms[0] = (time_us_32() - t0) / 1000u;
    phase(0);
    if (status == INSTALL_READY) {
      snprintf(line, sizeof line, "%s %lu\n", EFFECTS_FILE, (unsigned long)file_size(EFFECTS_FILE));
      status = append_manifest(line, 0);
    }
  }
  for (uint32_t song = 0; song < INSTALL_SONGS && status == INSTALL_READY; song++) {
    uint32_t k = 1u + 2u * song; /* its .ST8's output; k + 1 its .YM8's */
    int want[2] = {path == INSTALL_FOR_DMA && sizes[k] == 0, path == INSTALL_FOR_YM && sizes[k + 1u] == 0};
    if (!want[0] && !want[1]) continue;
    char st[16], ym[16];
    output_name(k, st);
    output_name(k + 1u, ym);
    const char *const names[2] = {st, ym};
    s_output = want[0] ? k : k + 1u;
    draw(1);
    uint32_t t0 = time_us_32();
    installStats.output = s_output;
    phase(1);
    work_begin(2u * (uint64_t)k_install_songs[song].size);
    for (int tries = 0;; tries++) {
      installStats.inflated = 0;
      status = make_song(b, song, want, names);
      if (status != INSTALL_RETRY) break;
      if (tries + 1 == INSTALL_TRIES) {
        status = INSTALL_READ_ERROR;
        break;
      }
      installStats.retries++;
      s_units_now = s_units_base; /* the song again, from its start */
    }
    work_end();
    installStats.output_ms[s_output] = (time_us_32() - t0) / 1000u;
    phase(0);
    DPRINTF("install: song %lu %d in %lu ms\n", (unsigned long)song, status,
            (unsigned long)installStats.output_ms[s_output]);
    for (int o = 0; o < 2 && status == INSTALL_READY; o++) {
      if (!want[o]) continue;
      snprintf(line, sizeof line, "%s %lu\n", names[o], (unsigned long)file_size(names[o]));
      status = append_manifest(line, 0);
    }
  }
  f_close(&s_zip_file);
  return status;
}

void install_refuse(int status, const char *folder, void (*pump)(void)) {
  static const uint16_t k_colours[PALETTE_ENTRIES] = {[0] = 0x000, [7] = 0x555, [14] = 0x770,
                                                      [15] = 0x777};
  char line[40];
  palette_set(k_colours);
  screen_start();
  /* 28 characters a line at most: the frame is 256 pixels wide. */
  if (status == INSTALL_NO_ZIP) {
    say(40, 14, "THE GAME'S FILES ARE MISSING");
    say(72, 15, "COPY THE ZIP OF THE AMIGA");
    say(84, 15, "PORT'S REPOSITORY,");
    say(96, 15, "GITHUB.COM/APOLKOSNIK/");
    say(108, 15, "CHORENSHA, INTO THE CARD'S");
    snprintf(line, sizeof line, "%s FOLDER.", folder);
    say(120, 15, line);
  } else if (status == INSTALL_BAD_MEMBER) {
    say(40, 14, "THE ZIP IS NOT THE ONE");
    say(52, 14, "THE GAME WAS TESTED WITH");
    say(84, 15, "A FILE IN IT IS MISSING OR");
    say(96, 15, "DIFFERENT: DOWNLOAD THE ZIP");
    say(108, 15, "OF COMMIT 40FEC7D AGAIN.");
  } else if (status == INSTALL_NO_PACK) {
    say(40, 14, "THE GAME'S IMAGES ARE MISSING");
    say(72, 15, "COPY THE .IMK AND .STK FILES");
    say(84, 15, "OF THIS RELEASE'S IMAGE PACK");
    snprintf(line, sizeof line, "INTO THE CARD'S %s", folder);
    say(96, 15, line);
    say(108, 15, "FOLDER.");
  } else if (status == INSTALL_OLD_PACK) {
    say(40, 14, "THE IMAGE PACK IS FOR");
    say(52, 14, "ANOTHER RELEASE");
    say(84, 15, "COPY THIS RELEASE'S .IMK AND");
    say(96, 15, ".STK FILES OVER THEM.");
  } else if (status == INSTALL_BAD_PACK) {
    say(40, 14, "THE IMAGE PACK IS DAMAGED");
    say(84, 15, "COPY THIS RELEASE'S .IMK AND");
    say(96, 15, ".STK FILES AGAIN.");
  } else if (status == INSTALL_READ_ERROR) {
    say(40, 14, "THE CARD'S DATA CAME BACK");
    say(52, 14, "DAMAGED, AGAIN AND AGAIN");
    say(84, 15, "TRY A SLOWER SD CARD SPEED");
    say(96, 15, "IN BOOSTER, OR ANOTHER CARD.");
  } else {
    say(40, 14, "THE CARD COULD NOT BE");
    say(52, 14, "WRITTEN");
    say(84, 15, "IT NEEDS ABOUT 60 MB FREE:");
    say(96, 15, "CHECK THE CARD, TRY AGAIN.");
  }
  say(152, 15, "PRESS A KEY: BOOSTER STARTS.");
  screen_show();
  ikbd_set_input_mode(IKBD_INPUT_KEYBOARD);
  ikbd_key_event_t key;
  while (ikbd_pop_key(&key)) {
  }
  for (;;) {
    pump();
    /* An ST that started again shows nothing until a new frame (laid out
     * for the copy it chose: pump follows its sound path). */
    if (st_session_consume_boot()) fb_republish();
    if (ikbd_pop_key(&key) && key.is_press) st_session_return_to_booster();
  }
}
