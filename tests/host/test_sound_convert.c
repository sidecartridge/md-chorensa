// units: rp/src/sound_convert.c
/* The sound converter (sound_convert.c): the ADPCM decoder against the
 * host's original (xl_host.c, its steps from pow()) over random data, whole
 * and in pieces; a .crm's blocks unpacked to mono in pieces of every size;
 * the resampler keeping a sine's frequency and level and a constant's level
 * at both rates; the song's plan. With CHORENSHA_CRM (a render) and
 * CHORENSHA_SONG (the same song made by music_convert.py with ffmpeg) set,
 * the converter's song is compared with ffmpeg's. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resample_table.h"
#include "sound_convert.h"
#include "test.h"

#define PI 3.14159265358979323846

static uint32_t s_seed = 1995;
static uint32_t rnd(void) {
  s_seed = s_seed * 1664525u + 1013904223u;
  return s_seed >> 8;
}

/* xl_host.c's decoder, as it was written. */
static void reference_decode(const uint8_t *in, uint32_t bytes, int8_t *out) {
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
    int nib = (in[k / 2u] >> ((k & 1u) ? 4 : 0)) & 15;
    signal += diff[step * 16 + nib];
    if (signal > 2047) signal = 2047;
    if (signal < -2048) signal = -2048;
    step += k_shift[nib & 7];
    if (step < 0) step = 0;
    if (step > 48) step = 48;
    out[k] = (int8_t)(signal >> 4);
  }
}

static void test_adpcm(void) {
  enum { BYTES = 50000 };
  static uint8_t in[BYTES];
  static int8_t want[2 * BYTES], got[2 * BYTES];
  for (uint32_t k = 0; k < BYTES; k++) in[k] = (uint8_t)rnd();
  reference_decode(in, BYTES, want);
  sc_adpcm_t a;
  sc_adpcm_start(&a);
  sc_adpcm_decode(&a, in, BYTES, got);
  CHECK(memcmp(want, got, sizeof want) == 0);
  sc_adpcm_start(&a); /* in pieces */
  for (uint32_t at = 0; at < BYTES;) {
    uint32_t n = 1u + rnd() % 777u;
    if (n > BYTES - at) n = BYTES - at;
    sc_adpcm_decode(&a, in + at, n, got + 2u * at);
    at += n;
  }
  CHECK(memcmp(want, got, sizeof want) == 0);
}

/* --- Unblocking ------------------------------------------------------------ */

typedef struct {
  int16_t *mono;
  uint32_t frames;
} gathered_t;

static int gather(void *arg, const int16_t *mono, uint32_t frames) {
  gathered_t *g = arg;
  memcpy(g->mono + g->frames, mono, frames * sizeof *mono);
  g->frames += frames;
  return 0;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static void test_unblock(void) {
  enum { INTRO = 5000, LOOP = 7001, BLOCK = 4096, FRAMES = INTRO + LOOP };
  static int8_t left[FRAMES], right[FRAMES];
  static int16_t mono[FRAMES];
  static uint8_t file[SC_CRM_HEADER + 2 * FRAMES];
  memset(file, 0, SC_CRM_HEADER);
  memcpy(file, "CRSM", 4);
  file[7] = 2; /* stereo */
  put_be32(file + 8, 22050), put_be32(file + 12, INTRO), put_be32(file + 16, LOOP);
  put_be32(file + 20, BLOCK);
  file[27] = 40, file[29] = 50; /* fade unit, level */
  uint8_t *body = file + SC_CRM_HEADER;
  for (uint32_t start = 0; start < FRAMES; start += BLOCK) {
    uint32_t n = FRAMES - start < BLOCK ? FRAMES - start : BLOCK;
    for (uint32_t k = 0; k < n; k++) {
      left[start + k] = (int8_t)rnd(), right[start + k] = (int8_t)rnd();
      body[2 * start + k] = (uint8_t)left[start + k];
      body[2 * start + n + k] = (uint8_t)right[start + k];
    }
  }
  sc_crm_t crm;
  CHECK_EQ(sc_crm_header(file, &crm), 0);
  CHECK_EQ(crm.intro, INTRO);
  CHECK_EQ(crm.loop, LOOP);
  CHECK_EQ(crm.block, BLOCK);
  CHECK_EQ(crm.fade_unit, 40);
  CHECK_EQ(crm.level, 50);
  static sc_unblock_t u;
  for (int pass = 0; pass < 2; pass++) { /* whole, then in pieces */
    gathered_t g = {mono, 0};
    sc_unblock_start(&u, &crm);
    for (uint32_t at = 0; at < 2u * FRAMES;) {
      uint32_t n = pass ? 1u + rnd() % 3000u : 2u * FRAMES;
      if (n > 2u * FRAMES - at) n = 2u * FRAMES - at;
      CHECK_EQ(sc_unblock(&u, body + at, n, gather, &g), 0);
      at += n;
    }
    CHECK_EQ(g.frames, FRAMES);
    int same = 1;
    for (uint32_t k = 0; k < FRAMES; k++) same &= mono[k] == left[k] + right[k];
    CHECK(same);
  }
  file[7] = 1; /* mono: refused */
  CHECK_EQ(sc_crm_header(file, &crm), -1);
}

/* --- The resampler --------------------------------------------------------- */

static double s_hz, s_level;
static int sine(void *ctx, uint32_t index) {
  (void)ctx;
  return (int)lround(2.0 * s_level * sin(2.0 * PI * s_hz * index / 22050.0));
}
static int constant(void *ctx, uint32_t index) {
  (void)ctx, (void)index;
  return 2 * 77;
}

/* The output's amplitude at `hz` (a single-bin DFT) over n samples. */
static double amplitude(const int8_t *y, uint32_t n, double hz, double rate) {
  double re = 0, im = 0;
  for (uint32_t k = 0; k < n; k++) {
    re += y[k] * cos(2.0 * PI * hz * k / rate);
    im += y[k] * sin(2.0 * PI * hz * k / rate);
  }
  return 2.0 * sqrt(re * re + im * im) / n;
}

static void test_resampler(void) {
  enum { N = 20000 };
  static int8_t y[N];
  static const uint32_t rates[2] = {RESAMPLE_ST_RATE, RESAMPLE_YM_RATE};
  for (int r = 0; r < 2; r++) {
    sc_resampler_t rs;
    s_hz = 1000.0, s_level = 100.0;
    CHECK_EQ(sc_resampler_start(&rs, rates[r], 0, sine, NULL), 0);
    for (uint32_t k = 0; k < N; k++) y[k] = sc_resampler_next(&rs);
    double a = amplitude(y + 100, N - 200, 1000.0, rates[r]);
    CHECK(a > 97.0 && a < 103.0);
    CHECK(amplitude(y + 100, N - 200, 1500.0, rates[r]) < 1.0); /* no other tone */
    /* A tone above the YM's 3.9 kHz Nyquist: filtered out there, kept for the DMA chip. */
    s_hz = 6000.0;
    CHECK_EQ(sc_resampler_start(&rs, rates[r], 0, sine, NULL), 0);
    double power = 0;
    for (uint32_t k = 0; k < N; k++) y[k] = sc_resampler_next(&rs);
    for (uint32_t k = 100; k < N; k++) power += (double)y[k] * y[k];
    double rms = sqrt(power / (N - 100));
    if (rates[r] == RESAMPLE_YM_RATE) CHECK(rms < 2.0);
    else CHECK(rms > 60.0);
    /* A constant stays itself; a start mid-stream lands where a run from 0 does. */
    CHECK_EQ(sc_resampler_start(&rs, rates[r], 0, constant, NULL), 0);
    for (uint32_t k = 0; k < 200; k++) y[k] = sc_resampler_next(&rs);
    CHECK_EQ(y[150], 77);
    s_hz = 1000.0;
    CHECK_EQ(sc_resampler_start(&rs, rates[r], 0, sine, NULL), 0);
    for (uint32_t k = 0; k < 5000; k++) y[k] = sc_resampler_next(&rs);
    CHECK_EQ(sc_resampler_start(&rs, rates[r], 4000, sine, NULL), 0);
    int same = 1;
    for (uint32_t k = 4000; k < 5000; k++) same &= sc_resampler_next(&rs) == y[k];
    CHECK(same);
  }
  sc_resampler_t rs;
  CHECK_EQ(sc_resampler_start(&rs, 44100, 0, sine, NULL), -1);
}

static void test_plan(void) {
  sc_crm_t crm = {22050, 100000, 300000, 4096, 40, 64};
  sc_song_t s;
  CHECK_EQ(sc_song_plan(&crm, RESAMPLE_ST_RATE, &s), 0);
  CHECK_EQ(s.intro, (uint32_t)llround(100000.0 * 25033 / 22050));
  CHECK_EQ(s.loop, (uint32_t)llround(300000.0 * 25033 / 22050));
  CHECK_EQ(s.loop_from, (uint32_t)llround(700000.0 * 25033 / 22050));
  CHECK_EQ(sc_song_source(&crm, 399999), 399999);
  CHECK_EQ(sc_song_source(&crm, 400000), 100000);
  CHECK_EQ(sc_song_source(&crm, 999999), 399999);
  CHECK_EQ(sc_song_source(&crm, 1000000), -1);
  sc_crm_t once = crm; /* no loop: the stream ends with the intro */
  once.loop = 0;
  CHECK_EQ(sc_song_source(&once, 99999), 99999);
  CHECK_EQ(sc_song_source(&once, 100000), -1);
  uint8_t h[SC_SONG_HEADER];
  sc_song_header(&crm, &s, h);
  CHECK(memcmp(h, "CRS8", 4) == 0);
  CHECK_EQ(h[4] | h[5] << 8, 25033 & 0xFFFF);
  CHECK_EQ(sc_song_plan(&crm, 44100, &s), -1);
}

/* --- A real song, against ffmpeg's ------------------------------------------ */

typedef struct {
  sc_crm_t crm;
  int16_t *mono;
} song_ctx_t;

static int song_frame(void *ctx, uint32_t index) {
  song_ctx_t *s = ctx;
  int32_t source = sc_song_source(&s->crm, index);
  return source < 0 ? 0 : s->mono[source];
}

static uint8_t *load(const char *path, uint32_t *size) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc((size_t)n);
  *size = (uint32_t)fread(data, 1, (size_t)n, f);
  fclose(f);
  return data;
}

static void test_real_song(const char *crm_path, const char *song_path) {
  uint32_t crm_size = 0, song_size = 0;
  uint8_t *crm_file = load(crm_path, &crm_size), *ffmpeg = load(song_path, &song_size);
  if (crm_file == NULL || ffmpeg == NULL) {
    printf("CHORENSHA_CRM / CHORENSHA_SONG not found, skipped\n");
    return;
  }
  song_ctx_t s;
  CHECK_EQ(sc_crm_header(crm_file, &s.crm), 0);
  s.mono = malloc((s.crm.intro + s.crm.loop) * sizeof *s.mono);
  gathered_t g = {s.mono, 0};
  static sc_unblock_t u;
  sc_unblock_start(&u, &s.crm);
  sc_unblock(&u, crm_file + SC_CRM_HEADER, crm_size - SC_CRM_HEADER, gather, &g);
  CHECK_EQ(g.frames, s.crm.intro + s.crm.loop);
  uint32_t rate = ffmpeg[4] | ffmpeg[5] << 8 | ffmpeg[6] << 16;
  sc_song_t plan;
  CHECK_EQ(sc_song_plan(&s.crm, rate, &plan), 0);
  uint8_t h[SC_SONG_HEADER];
  sc_song_header(&s.crm, &plan, h);
  uint32_t n = plan.intro + plan.loop;
  int8_t *y = malloc(n);
  sc_resampler_t rs;
  sc_resampler_start(&rs, rate, 0, song_frame, &s);
  for (uint32_t k = 0; k < plan.intro; k++) y[k] = sc_resampler_next(&rs);
  sc_resampler_start(&rs, rate, plan.loop_from, song_frame, &s);
  for (uint32_t k = 0; k < plan.loop; k++) y[plan.intro + k] = sc_resampler_next(&rs);
  /* ffmpeg's, sample against sample: the difference's power against the signal's. */
  uint32_t m = song_size - SC_SONG_HEADER < n ? song_size - SC_SONG_HEADER : n;
  double signal = 0, noise = 0;
  for (uint32_t k = 0; k < m; k++) {
    double a = (int8_t)ffmpeg[SC_SONG_HEADER + k], b = y[k];
    signal += a * a;
    noise += (a - b) * (a - b);
  }
  double snr = 10.0 * log10(signal / (noise > 0 ? noise : 1));
  printf("  %s at %u Hz: %u samples (ffmpeg's %u), against ffmpeg's %.1f dB\n", crm_path, rate, n,
         song_size - SC_SONG_HEADER, snr);
  CHECK(snr > 20.0);
  CHECK(n + 2u >= song_size - SC_SONG_HEADER && n <= song_size - SC_SONG_HEADER + 2u);
  free(y), free(s.mono), free(crm_file), free(ffmpeg);
}

int main(void) {
  test_adpcm();
  test_unblock();
  test_resampler();
  test_plan();
  const char *crm = getenv("CHORENSHA_CRM"), *song = getenv("CHORENSHA_SONG");
  if (crm && song) test_real_song(crm, song);
  TEST_END();
}
