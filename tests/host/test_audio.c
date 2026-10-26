// units: rp/src/audio.c
// cflags: -DAUDIO_YM_SOURCE_RATE_HZ=PROFILE_YM_RATE_HZ
/* The audio producer (audio.c) on the YM path against a simulated ST: its
 * cursor runs through the ring in the cart buffer, a VBL of samples a VBL,
 * on into the spill, and at every VBL the ST brings it back and reports it;
 * the writer's timer runs five times per VBL, and the main loop tops up the
 * FIFO once per frame unless the test stalls it.
 *
 * Everything the ST reads must be the next pair of the stream, or, when the
 * FIFO ran dry, the last pair held: never a pair the writer had not written
 * for it, never one skipped or repeated; late reads and underruns are
 * counted as the RP sees them. An ST that reads fewer samples a VBL than
 * the nominal rate (the machine does: the YM's wait states, the border
 * handlers, the IKBD's interrupts) gets the stream resampled to the rate it
 * achieves, which the RP measures from the reports. And the .YMS reader:
 * header checks, the wrap at the end of the file, a failed read.
 *
 * YM sources are built here at the output's nominal rate (cflags above), so
 * that a pair goes through unchanged and the ST's reads can be compared
 * pair by pair; the firmware's sources are at 5,585 Hz and resampled, as
 * the PCM triangle below is. */
#include <string.h>

#include "audio.h"
#include "cart_shared.h"
#include "ff.h"
#include "pico/time.h"
#include "profile.h"
#include "test.h"

#define VBL_US 20000u
#define WRITER_RUNS_PER_VBL 5
#define RING CART_AUDIO_YM_RING_BYTES
#define SB PROFILE_YM_SAMPLE_BYTES
#define NOMINAL PROFILE_YM_SAMPLES_PER_VBL /* samples a VBL at the nominal rate */

unsigned int __rom_in_ram_start__[0x10000 / sizeof(unsigned int)];
extern uint32_t audioSlicesWritten, audioUnderruns, audioLateSlices, audioYmRateHz;
#if PROFILE_YM_MOVEP
extern const uint8_t audioYmTriples[256][3];
#endif

/* --- The RP's clock, timer and ROM3 ring --------------------------------- */

static uint32_t now_us;
static repeating_timer_t *writer;
static int st_report = -1; /* the latest position report, -1: none */

uint32_t time_us_32(void) { return now_us; }

/* A plain ST said hello: the YM output. */
uint32_t st_session_hellos(void) { return 0; }
uint8_t st_session_machine(void) { return 0; }

bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t callback,
                            void *user_data, repeating_timer_t *out) {
  out->delay_us = delay_us;
  out->callback = callback;
  out->user_data = user_data;
  writer = out;
  return true;
}

bool commemul_latest(uint16_t mask, uint16_t match, uint32_t max_back,
                     uint16_t *sample) {
  (void)max_back;
  if (st_report < 0 || (match & mask) != CART_ROM3_AUDIO_POS_WINDOW) {
    return false;
  }
  *sample = (uint16_t)(CART_ROM3_AUDIO_POS_WINDOW | (uint16_t)st_report);
  return true;
}

/* --- Files in memory ------------------------------------------------------ */

static const BYTE *file_data;
static FSIZE_t file_size;
static const char *file_name;
static int fail_next_reads; /* the next reads fail, as a pulled card does */

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  (void)mode;
  if (!file_name || strcmp(path, file_name) != 0) return FR_NO_FILE;
  fp->data = file_data;
  fp->size = file_size;
  fp->pos = 0;
  return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  if (fail_next_reads > 0) {
    fail_next_reads--;
    *br = 0;
    return FR_DISK_ERR;
  }
  UINT n = fp->pos + btr <= fp->size ? btr : (UINT)(fp->size - fp->pos);
  memcpy(buff, fp->data + fp->pos, n);
  fp->pos += n;
  *br = n;
  return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
  fp->pos = ofs;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  (void)fp;
  return FR_OK;
}

/* --- The ST ---------------------------------------------------------------- */

static uint8_t *buffer(void) {
  return (uint8_t *)__rom_in_ram_start__ + CART_AUDIO_BUFFER_OFFSET;
}

static uint32_t st_vbl;               /* the ST's VBLs */
static uint32_t cursor;               /* bytes into the buffer: ring, then spill */
static uint32_t st_per_vbl = NOMINAL; /* the samples the ST writes a VBL */
static uint8_t played[300000][2];     /* every pair the ST read, in order */
static uint32_t n_played;
static int bad_registers;             /* at 50 fps: a register byte not 8, 9, 10 */

#if PROFILE_YM_MOVEP
static const uint8_t k_pairs[64][2];
/* At 50 fps the ST reads three channels' volumes: the table's pair they
 * stand for, through the 8-bit sample (the first with those volumes) whose
 * upper 6 bits index the pairs, as the two-channel table does. (255, 255)
 * when no sample has them. */
static void pair_of_triple(const uint8_t *t, uint8_t out[2]) {
  for (uint32_t v = 0; v < 256u; v++) {
    if (audioYmTriples[v][0] == t[0] && audioYmTriples[v][1] == t[1] &&
        audioYmTriples[v][2] == t[2]) {
      out[0] = k_pairs[v >> 2][0];
      out[1] = k_pairs[v >> 2][1];
      return;
    }
  }
  out[0] = out[1] = 255;
}
#endif

/* The ST reads `n` samples at its cursor. */
static void st_read(uint32_t n) {
  const uint8_t *b = buffer();
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *p = b + cursor;
#if PROFILE_YM_MOVEP
    const uint8_t volumes[3] = {p[0], p[2], p[4]};
    pair_of_triple(volumes, played[n_played]);
    if (p[1] != CART_YM_REG_VOL_A || p[3] != CART_YM_REG_VOL_B || p[5] != CART_YM_REG_VOL_C) {
      bad_registers++;
    }
#else
    played[n_played][0] = p[0];
    played[n_played][1] = p[1];
#endif
    n_played++;
    cursor += SB;
  }
}

/* One frame: the VBL (the cursor past the ring comes back by the ring, and
 * is reported unless `report` is off), then 20 ms in which the ST reads its
 * samples in five parts, the writer runs every 4 ms, and the main loop's
 * top-up comes unless stalled. */
static void frame_with(bool main_loop, bool writer_runs, bool report) {
  st_vbl++;
  if (cursor >= RING) cursor -= RING;
  CHECK(cursor < RING);
  if (report) st_report = (int)(cursor / CART_AUDIO_POS_UNIT);
  uint32_t left = st_per_vbl;
  for (int i = 0; i < WRITER_RUNS_PER_VBL; i++) {
    uint32_t part = left / (uint32_t)(WRITER_RUNS_PER_VBL - i);
    st_read(part);
    left -= part;
    now_us += VBL_US / WRITER_RUNS_PER_VBL;
    if (writer_runs) writer->callback(writer);
    if (i == 2 && main_loop) audio_render_frame();
  }
}

static void frame(bool main_loop, bool writer_runs) { frame_with(main_loop, writer_runs, true); }

/* --- The stream ------------------------------------------------------------ */

/* The table's pairs, which the output's table maps back to themselves at the
 * nominal rate (no resampling between equal rates), so what the ST reads
 * can be compared pair by pair. */
static const uint8_t k_pairs[64][2] = {
    {0, 0},  {0, 2},  {1, 2},  {2, 2},  {2, 3},  {1, 4},  {2, 4},  {2, 5},
    {0, 6},  {2, 6},  {3, 6},  {4, 6},  {2, 7},  {4, 7},  {5, 7},  {2, 8},
    {3, 8},  {4, 8},  {5, 8},  {2, 9},  {3, 9},  {4, 9},  {5, 9},  {6, 9},
    {7, 9},  {3, 10}, {4, 10}, {5, 10}, {6, 10}, {7, 10}, {0, 11}, {1, 11},
    {2, 11}, {4, 11}, {5, 11}, {6, 11}, {7, 11}, {8, 11}, {8, 11}, {9, 11},
    {9, 11}, {0, 12}, {1, 12}, {2, 12}, {3, 12}, {4, 12}, {5, 12}, {6, 12},
    {8, 12}, {8, 12}, {9, 12}, {9, 12}, {9, 12}, {10, 12}, {0, 13}, {2, 13},
    {3, 13}, {4, 13}, {5, 13}, {6, 13}, {7, 13}, {8, 13}, {8, 13}, {9, 13},
};

/* What the ST plays for each of the table's pairs: at 50 fps the pair its
 * three channels' volumes decode to (the RP plays a pair as the middle of
 * its entries, and towards the ends of the triples' table neighbouring
 * pairs share their volumes), else the pair itself. */
static uint8_t k_seen[64][2];

static void make_seen(void) {
  for (uint32_t i = 0; i < 64u; i++) {
#if PROFILE_YM_MOVEP
    uint32_t sum = 0, count = 0;
    for (uint32_t j = 0; j < 64u; j++) {
      if (k_pairs[j][0] == k_pairs[i][0] && k_pairs[j][1] == k_pairs[i][1]) sum += j, count++;
    }
    pair_of_triple(audioYmTriples[(sum + count / 2u) / count * 4u + 2u], k_seen[i]);
#else
    k_seen[i][0] = k_pairs[i][0];
    k_seen[i][1] = k_pairs[i][1];
#endif
  }
}

#define STREAM_PAIRS 1728u /* a whole number of the 64-pair period, not of VBLs */
static uint8_t stream[STREAM_PAIRS * 2];

/* Pair k of the stream: an entry of the table, every entry in turn with an
 * odd stride, so that a run of a few pairs says where in the 64 it is. */
static uint32_t stream_index(uint32_t k) { return (k * 7u) % 64u; }

static void make_stream(void) {
  for (uint32_t k = 0; k < STREAM_PAIRS; k++) {
    const uint8_t *p = k_pairs[stream_index(k)];
    stream[2 * k] = p[0];
    stream[2 * k + 1] = p[1];
  }
}

static bool pair_is(uint32_t at, uint32_t k) {
  const uint8_t *p = k_seen[stream_index(k)];
  return played[at][0] == p[0] && played[at][1] == p[1];
}

/* From `at`, the played pairs follow the stream from pair `k` on, for `n`
 * pairs. */
static bool follows(uint32_t at, uint32_t k, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (!pair_is(at + i, k + i)) {
      const uint8_t *p = k_seen[stream_index(k + i)];
      fprintf(stderr, "played %u is (%u, %u), not pair %u (%u, %u)\n",
              (unsigned)(at + i), played[at + i][0], played[at + i][1],
              (unsigned)(k + i), p[0], p[1]);
      return false;
    }
  }
  return true;
}

/* The run of the stream the played pairs end with: its start `at` and the
 * pair `k` there (k is counted from a high base, so that it never goes
 * below zero); false when the last pairs are not the stream at all. The
 * stream's pairs repeat every 64, so eight of them in a row say where. */
static bool stream_tail(uint32_t *at, uint32_t *k) {
  if (n_played < 8) return false;
  uint32_t end = n_played - 8, kend = 0;
  bool found = false;
  for (uint32_t c = 0; c < 64 && !found; c++) {
    bool ok = true;
    for (uint32_t j = 0; j < 8 && ok; j++) ok = pair_is(end + j, c + j);
    if (ok) found = true, kend = 64u * 100000u + c;
  }
  if (!found) return false;
  uint32_t a = end, kk = kend;
  while (a > 0 && pair_is(a - 1, kk - 1)) a--, kk--;
  *at = a;
  *k = kk;
  return true;
}

/* The pairs the stream holds between two of its runs... the table's index
 * of a played pair (the first entry with it), for the resampled phase. */
static int index_of(uint32_t at) {
  for (int i = 0; i < 64; i++) {
    if (k_pairs[i][0] == played[at][0] && k_pairs[i][1] == played[at][1]) return i;
  }
  return -1;
}

/* --- A PCM triangle, for the resampled phase --------------------------------- */

/* One more each source sample from -128 up to 127, then one less back down:
 * no jump for the resampler to interpolate across. */
static int ramp_value = -128, ramp_dir = 1;
static void ramp_cb(int8_t *buf, uint32_t samples) {
  for (uint32_t i = 0; i < samples; i++) {
    buf[i] = (int8_t)ramp_value;
    if (ramp_value == 127) ramp_dir = -1;
    if (ramp_value == -128) ramp_dir = 1;
    ramp_value += ramp_dir;
  }
}

int main(void) {
  make_seen();
  make_stream();
  audio_init();
  CHECK(writer != NULL);
  CHECK_EQ(writer->delay_us < 0 ? -writer->delay_us : writer->delay_us,
           VBL_US / WRITER_RUNS_PER_VBL);
  audio_play_loop(stream, sizeof stream);
  audio_render_frame(); /* the FIFO fills before the ST starts */

  /* The ST starts: silence until the writer has seen its first report and
   * written ahead of it, then the stream, pair after pair. */
  uint32_t at0, k0;
  for (int f = 0; f < 204; f++) frame(true, true);
  CHECK(stream_tail(&at0, &k0));
  CHECK(at0 < 4u * NOMINAL); /* the stream within the first frames */
  CHECK_EQ(audioLateSlices, 0);
  CHECK_EQ(audioUnderruns, 0);
  CHECK_EQ(bad_registers, 0);
  CHECK_EQ(audioYmRateHz, PROFILE_YM_RATE_HZ);

  /* The main loop misses 3 frames: 80 ms between two of its top-ups, what
   * the FIFO covers (1, 40 ms, at 50 fps, where it holds three channels'
   * volumes in the DMA chip's memory: audio.c). Nothing is held; the
   * stream goes on. */
  for (int f = 0; f < (PROFILE_YM_MOVEP ? 1 : 3); f++) frame(false, true);
  for (int f = 0; f < 10; f++) frame(true, true);
  CHECK(follows(at0, k0, n_played - at0));
  CHECK_EQ(audioUnderruns, 0);

  /* It misses 8: the FIFO runs dry, and the pairs written then hold the
   * last one; the stream resumes where it stopped. */
  uint32_t before = n_played;
  for (int f = 0; f < 8; f++) frame(false, true);
  for (int f = 0; f < 10; f++) frame(true, true);
  CHECK(audioUnderruns > 0);
  uint32_t good = before;
  while (good < n_played && pair_is(good, k0 + (good - at0))) good++;
  CHECK(good < n_played);
  uint32_t held = good;
  while (held < n_played && played[held][0] == played[good - 1][0] &&
         played[held][1] == played[good - 1][1]) {
    held++;
  }
  CHECK(held > good + 50); /* a run of the held pair */
  CHECK(held < n_played);
  /* The stream resumes at `held` with the pair `good` was due to. */
  k0 = k0 + (good - at0);
  at0 = held;
  CHECK(follows(at0, k0, n_played - at0));
  CHECK_EQ(audioLateSlices, 0);
  uint32_t underruns = audioUnderruns;

  /* The writer misses 2 frames (interrupts off for a flash write, about
   * 50 ms): the ST reads past what was written, counted as late. Then the
   * stream goes on: the pairs it read meanwhile were old, and after them it
   * is the stream again, from the FIFO on. */
  for (int f = 0; f < 2; f++) frame(true, false);
  for (int f = 0; f < 13; f++) frame(true, true);
  CHECK(audioLateSlices >= 1);
  uint32_t late = audioLateSlices;
  CHECK(stream_tail(&at0, &k0));
  CHECK(n_played - at0 >= 10u * st_per_vbl - 2u * NOMINAL);

  /* The ST reports only every other VBL (a VBL handler held up): the writer
   * estimates the cursor from the rate; nothing is late. */
  for (int f = 0; f < 30; f++) frame_with(true, true, (f & 1) == 0);
  CHECK(follows(at0, k0, n_played - at0));
  CHECK_EQ(audioLateSlices, late);

  /* The ST leaves (GEM): no new report for longer than the timeout. Then it
   * boots again with its cursor at the ring's start: silence (the writer
   * starts over), then the stream goes on from where the FIFO is. */
  for (int i = 0; i < 60; i++) {
    now_us += 4000u;
    writer->callback(writer);
  }
  audio_render_frame();
  cursor = 0;
  for (int f = 0; f < 26; f++) frame(true, true);
  CHECK(stream_tail(&at0, &k0));
  CHECK(n_played - at0 >= 20u * st_per_vbl);
  CHECK_EQ(audioLateSlices, late);
  CHECK_EQ(audioUnderruns, underruns);
  CHECK_EQ(bad_registers, 0);

  /* Again, with the cursor five samples in when the writer first hears of
   * it: at 50 fps not on the report's unit (six-byte samples), so the
   * writer starts from the sample at or before the report, on the ST's
   * samples: every register where the ST writes one. */
  for (int i = 0; i < 60; i++) {
    now_us += 4000u;
    writer->callback(writer);
  }
  audio_render_frame();
  cursor = 5u * SB;
  for (int f = 0; f < 26; f++) frame(true, true);
  CHECK(stream_tail(&at0, &k0));
  CHECK(n_played - at0 >= 20u * st_per_vbl);
  CHECK_EQ(audioLateSlices, late);
  CHECK_EQ(audioUnderruns, underruns);
  CHECK_EQ(bad_registers, 0);

  /* A slower ST: 3.5% fewer samples a VBL, as the machine writes with the
   * YM's wait states and the IKBD's interrupts. Within a few seconds the RP
   * measures the rate it achieves, within 1%, and resamples to it. A PCM
   * triangle (one more or less each source sample at the nominal rate, so a
   * table index on every four) then advances by the rate's ratio: counted
   * over 100 frames by the index steps the ST reads, within 2%. */
  st_per_vbl = NOMINAL * 965u / 1000u;
  audio_set_pcm_callback(ramp_cb, PROFILE_YM_RATE_HZ);
  for (int f = 0; f < 500; f++) frame(true, true); /* the rate's windows smooth it in */
  uint32_t achieved = st_per_vbl * 1000000u / VBL_US;
  CHECK(audioYmRateHz > achieved * 99u / 100u && audioYmRateHz < achieved * 101u / 100u);
  CHECK_EQ(audioLateSlices, late);
  CHECK_EQ(audioUnderruns, underruns);
  uint32_t start = n_played;
  for (int f = 0; f < 100; f++) frame(true, true);
  uint32_t steps = 0;
  int prev = index_of(start);
  CHECK(prev >= 0);
  for (uint32_t a = start + 1; a < n_played; a++) {
    int cur = index_of(a);
    CHECK(cur >= 0);
    int d = cur > prev ? cur - prev : prev - cur;
    CHECK(d <= 3); /* the table's duplicate entries read as the first of them */
    steps += (uint32_t)d;
    prev = cur;
  }
  uint32_t expected = (uint32_t)((uint64_t)(n_played - start) * NOMINAL / st_per_vbl / 4u);
  CHECK(steps > expected * 98u / 100u && steps < expected * 102u / 100u);
  st_per_vbl = NOMINAL;
  audio_play_loop(stream, sizeof stream);
  for (int f = 0; f < 500; f++) frame(true, true);
  CHECK_EQ(audioYmRateHz, PROFILE_YM_RATE_HZ);
  /* The rate came back to the nominal one over a few of its one-second
   * windows; since then the stream goes through as it is. */
  CHECK(stream_tail(&at0, &k0));
  CHECK(n_played - at0 >= 20u * NOMINAL);

  /* .YMS files: the header is checked, and a failure keeps the loop. */
  static BYTE yms[16 + 2 * 300]; /* the body ends inside a VBL */
  static BYTE seen_body[2 * 300]; /* what the ST plays for it */
  memcpy(yms, "YMS1", 4);
  uint32_t rate = AUDIO_YM_SOURCE_RATE_HZ, len = sizeof yms - 16;
  memcpy(yms + 4, &rate, 4); /* the host is little-endian, as the file */
  memcpy(yms + 8, &len, 4);
  yms[12] = 1; /* dual-ghost */
  /* Table pairs in a pseudo-random order, no run of 20 repeated anywhere
   * in what the ST plays for the body (checked), so that a run of them says
   * where in the body it is. */
  uint32_t x = 0x9E3779B9u;
  for (unsigned k = 0; k < 300; k++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    const uint8_t *p = k_pairs[x & 63u];
    yms[16 + 2 * k] = p[0];
    yms[17 + 2 * k] = p[1];
    seen_body[2 * k] = k_seen[x & 63u][0];
    seen_body[2 * k + 1] = k_seen[x & 63u][1];
  }
  for (unsigned a = 0; a < 300; a++) {
    for (unsigned b = a + 1; b < 300; b++) {
      bool same = true;
      for (unsigned j = 0; j < 20 && same; j++) {
        same = seen_body[2 * ((a + j) % 300)] == seen_body[2 * ((b + j) % 300)] &&
               seen_body[2 * ((a + j) % 300) + 1] == seen_body[2 * ((b + j) % 300) + 1];
      }
      CHECK(!same);
    }
  }
  file_name = "A.YMS";
  file_data = yms;
  file_size = sizeof yms;
  CHECK_EQ(audio_play_yms_file("B.YMS"), -1); /* no such file */
  yms[0] = 'X';
  CHECK_EQ(audio_play_yms_file("A.YMS"), -1); /* bad magic */
  yms[0] = 'Y';
  uint32_t other = rate - 33;
  memcpy(yms + 4, &other, 4);
  CHECK_EQ(audio_play_yms_file("A.YMS"), -1); /* another rate */
  memcpy(yms + 4, &rate, 4);
  yms[12] = 2;
  CHECK_EQ(audio_play_yms_file("A.YMS"), -1); /* another mode */
  yms[12] = 1;
  file_size = 10;
  CHECK_EQ(audio_play_yms_file("A.YMS"), -1); /* short header */
  file_size = sizeof yms;
  for (int f = 0; f < 10; f++) frame(true, true);
  CHECK(follows(at0, k0, n_played - at0));

  /* Streaming: once the loop's pairs already queued have played, every pair
   * is the next of the file's body, which wraps at its end to the body's
   * start, never to the header. */
  CHECK_EQ(audio_play_yms_file("A.YMS"), 0);
  const BYTE *body = seen_body; /* as the ST plays it */
  const uint32_t body_pairs = len / 2;
  for (int f = 0; f < 40; f++) frame(true, true);
  /* The body's run the played pairs end with. */
  uint32_t end = n_played - 20, kend = body_pairs;
  for (uint32_t c = 0; c < body_pairs && kend == body_pairs; c++) {
    bool ok = true;
    for (uint32_t j = 0; j < 20 && ok; j++) {
      ok = played[end + j][0] == body[2 * ((c + j) % body_pairs)] &&
           played[end + j][1] == body[2 * ((c + j) % body_pairs) + 1];
    }
    if (ok) kend = c;
  }
  CHECK(kend < body_pairs);
  uint32_t ystart = end, ymsn = kend + 10u * body_pairs;
  while (ystart > 0 && played[ystart - 1][0] == body[2 * ((ymsn - 1) % body_pairs)] &&
         played[ystart - 1][1] == body[2 * ((ymsn - 1) % body_pairs) + 1]) {
    ystart--, ymsn--;
  }
  CHECK(n_played - ystart > 3 * body_pairs); /* crossed the end of the file several times */
  CHECK(ystart > n_played - 40u * NOMINAL);  /* and started in these frames */

  /* A failed read (a pulled card) gives a chunk of silence, and the next
   * read starts again at the body's start. */
  fail_next_reads = 1;
  uint32_t fstart = n_played;
  for (int f = 0; f < 12; f++) frame(true, true);
  uint32_t silence = n_played;
  for (uint32_t a = fstart; a + 8 < n_played; a++) {
    bool zero = true;
    for (uint32_t j = 0; j < 8 && zero; j++) zero = played[a + j][0] == 0 && played[a + j][1] == 0;
    if (zero) {
      silence = a;
      break;
    }
  }
  CHECK(silence < n_played);
  bool found = false;
  for (uint32_t a = silence; a + 20 < n_played && !found; a++) {
    bool ok = true;
    for (uint32_t j = 0; j < 20 && ok; j++) {
      ok = played[a + j][0] == body[2 * j] && played[a + j][1] == body[2 * j + 1];
    }
    found = ok;
  }
  CHECK(found);
  CHECK_EQ(audioLateSlices, late);

  CHECK_EQ(bad_registers, 0);
  TEST_END();
}
