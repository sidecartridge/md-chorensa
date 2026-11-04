/**
 * File: audio.c
 * Description: The app's sound, to the YM or to the DMA sound chip.
 *
 * Two outputs, one per machine (the ST's hello says which):
 *   - the YM: the ST's cursor runs through a ring of samples in the cart
 *     buffer at CART_AUDIO_BUFFER_OFFSET (CART_AUDIO_YM_RING_BYTES, four
 *     bytes a sample at 50 fps, the (vA, vB) pair at 25: cart_shared.h),
 *     written by its copy loop or by Timer-D, and its VBL handler reports
 *     where the cursor is (CART_ROM3_AUDIO_POS_WINDOW) after bringing it
 *     back from the spill past the ring's end, which mirrors the ring's
 *     start. The RP writes ahead of the cursor, never at it, and resamples
 *     the app's sound to the rate the ST achieves, which the reports
 *     measure (audioYmRateHz): no sample is skipped or held, whatever the
 *     ST's frame does;
 *   - the DMA sound chip of an STE or a Mega STE: it plays 8-bit samples at
 *     the profile's rate from a ring in ST RAM, which the ST fills every VBL
 *     from the cart buffer, its mirror, up to the profile's lead
 *     (PROFILE_DMA_LEAD) ahead of where it reports the chip plays
 *     (CART_ROM3_AUDIO_POS_WINDOW). The RP writes the mirror further ahead
 *     still, never where the ST copies.
 * The rates are the app's profile's (profile.h): the YM at 7,777 Hz
 * (50 fps) or 21,943 Hz (25 fps), the DMA chip at 25,033 Hz in both.
 * The ST picks the output at boot by the same rule the RP applies at the
 * hello (select_output): the DMA chip when the machine has one, unless the
 * app asked for the YM (audio_prefer_ym; CART_AUDIO_OUT_OFFSET).
 *
 * Two stages, so that the sound does not depend on the app's frame loop:
 *   - audio_render_frame(), from the main loop, tops up a small RAM FIFO in
 *     the output's format with the app's source: YM pairs (a loop in flash, a
 *     .YMS file on the SD card, the app's callback) or 8-bit PCM (a loop, a
 *     callback), converted when the output wants the other (the 64-entry
 *     two-channel table both ways, linear resampling between the rates);
 *   - a repeating timer interrupt (audio_writer) follows the ST's reports and
 *     moves samples from the FIFO into the cart buffer ahead of the ST.
 * The main loop may go about AUDIO_FIFO_VBLS VBLs between two top-ups
 * without the sound noticing (80 ms). When the FIFO is empty the output
 * holds the last sample (an underrun, counted), never stale data.
 *
 * See audio.h for the public API.
 */

#include "audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cart_shared.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "profile.h"
#include "st_session.h"

/* A PAL VBL, nominally: the period the sample arithmetic below is for. */
#define AUDIO_FRAME_PERIOD_US 20000u

/* The YM path's ring: a VBL of samples at the nominal rate (155.4 at
 * 7,777 Hz, 438.2 at 21,943), each AUDIO_YM_SAMPLE_BYTES in the ring (the
 * profile, profile.h). The writer keeps the ring written AUDIO_YM_AHEAD
 * samples ahead of where the cursor is now (the last report plus what the
 * ST read since, at the rate it achieves): the writer's period and how late
 * it sees a report (4 ms each) and a VBL more. The first report, or the ST
 * overtaking the writer, starts it AUDIO_YM_LEAD samples ahead. The spill
 * after the ring mirrors its first AUDIO_YM_SPILL bytes: the cursor reads
 * on past the ring's end until the VBL brings it back. */
#define AUDIO_YM_SAMPLE_BYTES PROFILE_YM_SAMPLE_BYTES
#define AUDIO_YM_SAMPLES_PER_VBL PROFILE_YM_SAMPLES_PER_VBL
#define AUDIO_YM_RING CART_AUDIO_YM_RING_BYTES
#define AUDIO_YM_SPILL CART_AUDIO_YM_SPILL_BYTES
#define AUDIO_YM_AHEAD (AUDIO_YM_SAMPLES_PER_VBL * 3u / 2u)
#define AUDIO_YM_LEAD (AUDIO_YM_SAMPLES_PER_VBL / 2u)
/* The estimate goes at most this far past the last report. */
#define AUDIO_YM_EST_MAX_US (3u * AUDIO_FRAME_PERIOD_US)
/* The rate the ST achieves is measured over this many of its VBLs (its
 * reports come one a VBL, 19.979 ms of its crystal each; a report seen
 * after two or three VBLs, the writer having been stalled, counts as that
 * many): the samples it read in them, over their time. The first window
 * sets the rate, the next ones smooth it by halves (a change settles within
 * a second or two). A window is within a sample or two: the time is the
 * ST's, not the RP's clock at whatever moment the writer saw each report,
 * which wandered by a percent. */
#define AUDIO_YM_RATE_VBLS 25u
#define AUDIO_VBL_PERIOD_US 19979u
_Static_assert(AUDIO_YM_RING + AUDIO_YM_SPILL <= CART_AUDIO_BUFFER_SIZE,
               "the ring and its spill fit the buffer");
_Static_assert(AUDIO_YM_RING % AUDIO_YM_SAMPLE_BYTES == 0 &&
                   AUDIO_YM_SPILL % AUDIO_YM_SAMPLE_BYTES == 0,
               "whole samples");
_Static_assert(AUDIO_YM_SPILL >= AUDIO_YM_SAMPLES_PER_VBL * AUDIO_YM_SAMPLE_BYTES * 11u / 10u,
               "the spill holds a VBL of samples and a tenth more");
/* A VBL and a half more than what is written ahead: the writer's estimate
 * of the cursor would have to be off by that much (the ST's VBL handler
 * held up for longer than a VBL) to overwrite samples not yet played. */
_Static_assert(AUDIO_YM_RING / AUDIO_YM_SAMPLE_BYTES >=
                   AUDIO_YM_AHEAD + AUDIO_YM_SAMPLES_PER_VBL * 3u / 2u,
               "the ring holds what is written ahead and a VBL and a half more");

/* The FIFO between the app's source and the writer, in VBLs of samples:
 * the longest the main loop may go between two top-ups. More delays the
 * sound as much: on the YM up to AUDIO_FIFO_VBLS VBLs and AUDIO_YM_AHEAD
 * samples from the source to the ST. */
#ifndef AUDIO_FIFO_VBLS
#define AUDIO_FIFO_VBLS 4u
#endif
/* A sample's volumes: at 50 fps the YM's three channels (the copy writes
 * them with a movep.l and a movep.w), at 25 fps two (Timer-D's pair). */
#if PROFILE_YM_MOVEP
#define AUDIO_YM_VOICES 3u
#else
#define AUDIO_YM_VOICES 2u
#endif
/* The YM's FIFO, in samples: AUDIO_FIFO_VBLS VBLs and one more, or as many
 * as the DMA chip's FIFO, whose memory it shares, holds of its volumes:
 * at 50 fps, three channels' worth, 2.7 VBLs (the main loop may then go 40
 * ms between two top-ups, not 80). */
#define AUDIO_YM_FIFO_WANT ((AUDIO_FIFO_VBLS + 1u) * AUDIO_YM_SAMPLES_PER_VBL)
#define AUDIO_YM_FIFO_FITS (AUDIO_DMA_FIFO_BYTES / AUDIO_YM_VOICES)
#define AUDIO_YM_FIFO_SAMPLES \
  (AUDIO_YM_FIFO_WANT < AUDIO_YM_FIFO_FITS ? AUDIO_YM_FIFO_WANT : AUDIO_YM_FIFO_FITS)

/* The writer runs this often; the ST's report is seen at most this long
 * after its VBL, well before the samples are needed. */
#define AUDIO_WRITER_PERIOD_US 4000
/* How far back in the ROM3 ring the writer looks for the latest report: a
 * frame adds a few samples (the reports, the blit acknowledgement, keys). */
#define AUDIO_REPORT_LOOKBACK 64u
/* No new report for this long: the ST left userfw (GEM, a reset). The next
 * report starts the count again. */
#define AUDIO_REPORT_TIMEOUT_US 200000u

/* The YM output's rate: Timer-B's, set by userfw from the profile. */
#define AUDIO_NATIVE_RATE_HZ      PROFILE_YM_RATE_HZ

/* The rate of YM-pair sources: .YMS files, the built-in jingle, fill
 * callbacks. At another output rate they are resampled like PCM. (A host
 * test sets it to the output's, where a source goes through unchanged.) */
#ifndef AUDIO_YM_SOURCE_RATE_HZ
#define AUDIO_YM_SOURCE_RATE_HZ   5585u
#endif

/* The DMA output: its FIFO (500.7 samples a VBL at 25,033 Hz) is topped up
 * by the chunk; it shares the YM FIFO's memory. At 25 fps it holds five
 * VBLs, so the main loop, which tops it up every second VBL, may go four
 * between two top-ups; at 50 fps two and a half (1,280 bytes: what the
 * translated game's RAM leaves), so the main loop, which tops it up every
 * VBL, may go two. The
 * writer keeps the mirror written this far ahead of where
 * the chip plays now: its last report, plus the samples played since (a
 * missed VBL sends no report, and the chip goes on). At its next VBL the ST
 * copies up to PROFILE_DMA_LEAD ahead; the rest covers the writer's period
 * and how late it sees a report (4 ms each: 5/8 of a VBL together). */
#define AUDIO_DMA_FIFO_BYTES (PROFILE_VBLS_A_FRAME == 1 ? 1280u : 5u * PROFILE_DMA_BYTES_PER_VBL)
#define AUDIO_DMA_AHEAD (PROFILE_DMA_LEAD + PROFILE_DMA_BYTES_PER_VBL * 5u / 8u)
/* The estimate goes at most this far past the last report. */
#define AUDIO_DMA_EST_MAX_US (3u * AUDIO_FRAME_PERIOD_US)
#define AUDIO_DMA_RING_MASK (CART_AUDIO_DMA_RING_BYTES - 1u)


/* A PCM callback is asked for this many samples at a time. */
#define AUDIO_PCM_CHUNK 64u

typedef enum {
  AUDIO_OUT_NONE = 0, /* between two outputs: the writer waits */
  AUDIO_OUT_YM,
  AUDIO_OUT_DMA,
} audio_out_t;

static uint8_t *s_audio_buf;
static volatile audio_out_t s_out;
static bool s_prefer_ym;
static bool s_st_silent; /* audio_silence_st() */
static volatile bool s_paused; /* audio_pause(): the writer holds the level */
static uint32_t s_hellos_seen;

/* The app's source: YM pairs at AUDIO_YM_SOURCE_RATE_HZ, or PCM at a rate. */
static audio_fill_cb_t s_fill_cb;
static audio_pcm_cb_t s_pcm_cb;
static const uint8_t *s_ym_curve; /* audio_set_ym_curve(), or NULL */
static uint32_t s_pcm_rate;

/* The output's FIFO: volumes for the YM, samples for the DMA chip. One
 * output plays at a time, so they share the memory. */
static union {
  uint8_t ym[AUDIO_YM_FIFO_SAMPLES * AUDIO_YM_VOICES];
  int8_t dma[AUDIO_DMA_FIFO_BYTES];
} s_fifo_mem;
#define s_ym_fifo (s_fifo_mem.ym)
#define s_dma_fifo (s_fifo_mem.dma)
static volatile uint32_t s_ym_head; /* samples produced (main loop) */
static volatile uint32_t s_ym_tail; /* samples consumed (writer) */
static volatile uint32_t s_dma_head; /* samples produced (main loop) */
static volatile uint32_t s_dma_tail; /* samples consumed (writer) */

/* The YM writer's view of the ST. */
static bool s_ym_known;          /* the cursor's position is known */
static uint32_t s_ym_front;      /* the next byte of the ring the RP writes */
static uint32_t s_ym_prev_play;  /* the previous report's position, and when */
static uint32_t s_ym_prev_us;
static uint32_t s_ym_acc_bytes;  /* read by the ST since the rate was last taken */
static uint32_t s_ym_acc_vbls;   /* the ST's VBLs over the same reports */
static bool s_ym_rate_known;     /* a first window has set the rate */
static uint16_t s_last_report = 0xFFFFu; /* none yet: the first one is fresh */
static uint32_t s_last_report_us;
static uint8_t s_hold[AUDIO_YM_VOICES]; /* the last sample written: what an underrun holds */

/* The DMA writer's view of the ST. */
static bool s_dma_known;
static uint32_t s_dma_front; /* the next offset of the ring the RP writes */
static int8_t s_dma_hold;

static repeating_timer_t s_writer_timer;

/* Readable over SWD by their symbols (tools/dev/swd.py counters). */
uint32_t audioSlicesWritten; /* VBLs of samples the writer followed */
uint32_t audioUnderruns;     /* writes of a held sample: the FIFO was empty */
uint32_t audioLateSlices;    /* the ST played or copied what the RP had not written */
uint32_t audioOutput;        /* 1: the YM, 2: the DMA sound chip */
uint32_t audioYmRateHz;      /* the rate the ST achieves on the YM, from its reports */

static bool audio_writer(repeating_timer_t *timer);

/* Static-loop convenience state. audio_play_loop() points
 * s_fill_cb at audio_loop_cb and stores the source span here. */
static const uint8_t *s_loop_data;
static uint32_t s_loop_bytes;
static uint32_t s_loop_pos;

/* audio_play_pcm_loop()'s span. */
static const int8_t *s_pcm_loop_data;
static uint32_t s_pcm_loop_samples;
static uint32_t s_pcm_loop_pos;

/* .YMS-file streaming state. audio_play_yms_file() validates the
 * 16-byte header, stores the data offset, and installs audio_yms_cb
 * as the per-VBL fill callback. The file is kept open for the
 * lifetime of playback. */
#define AUDIO_YMS_HEADER_SIZE     16u
#define AUDIO_YMS_MODE_DUAL_GHOST 1u

static FIL s_yms_file;
static bool s_yms_open;
/* A read failed and has not succeeded since: logged once per streak. */
static bool s_yms_failing;
static FSIZE_t s_yms_data_offset;

/* --- PCM <-> YM pairs ---------------------------------------------------- */

/* The (vA, vB) pairs of the 1988 Ghostbusters demo's SAMPLE1 table, indexed
 * by the upper 6 bits of an unsigned 8-bit sample: tools/wav_to_ym4.py's
 * GHOSTBUSTERS_LUT (tests/host/test_audio_convert.c compares the two). */
static const uint8_t k_ghost_pairs[64][2] = {
    {0, 0},  {0, 2},  {1, 2},  {2, 2},  {2, 3},  {1, 4},  {2, 4},  {2, 5},
    {0, 6},  {2, 6},  {3, 6},  {4, 6},  {2, 7},  {4, 7},  {5, 7},  {2, 8},
    {3, 8},  {4, 8},  {5, 8},  {2, 9},  {3, 9},  {4, 9},  {5, 9},  {6, 9},
    {7, 9},  {3, 10}, {4, 10}, {5, 10}, {6, 10}, {7, 10}, {0, 11}, {1, 11},
    {2, 11}, {4, 11}, {5, 11}, {6, 11}, {7, 11}, {8, 11}, {8, 11}, {9, 11},
    {9, 11}, {0, 12}, {1, 12}, {2, 12}, {3, 12}, {4, 12}, {5, 12}, {6, 12},
    {8, 12}, {8, 12}, {9, 12}, {9, 12}, {9, 12}, {10, 12}, {0, 13}, {2, 13},
    {3, 13}, {4, 13}, {5, 13}, {6, 13}, {7, 13}, {8, 13}, {8, 13}, {9, 13},
};

#if AUDIO_YM_VOICES == 3
/* For each unsigned 8-bit sample the YM's three volumes (vA, vB, vC) whose
 * output, as measured on an ST, lies nearest to a straight line over
 * tools/ym_triples.py's span: about 8 bits. The ST ties the three outputs together, so the channels do not add;
 * tools/ym_triples.py fits this table to Paulo Simoes' measurements of every
 * combination (Hatari's ym2149_fixed_vol.h), which hold with the mixer's
 * tones off, as userfw.s sets it. Not static: the host tests read it. */
const uint8_t audioYmTriples[256][3] = {
    {0, 0, 0}, {2, 0, 0}, {3, 0, 0}, {2, 1, 1}, {4, 1, 0}, {3, 3, 1}, {5, 1, 1}, {4, 3, 2},
    {4, 4, 2}, {5, 5, 0}, {5, 4, 3}, {5, 4, 4}, {6, 4, 3}, {7, 4, 1}, {6, 6, 1}, {7, 5, 2},
    {8, 3, 1}, {7, 6, 2}, {7, 5, 5}, {8, 5, 2}, {7, 7, 3}, {9, 4, 0}, {9, 3, 3}, {9, 5, 1},
    {8, 7, 3}, {9, 6, 1}, {8, 7, 5}, {10, 1, 0}, {8, 7, 6}, {8, 8, 4}, {8, 8, 5}, {9, 8, 1},
    {8, 8, 6}, {10, 6, 1}, {8, 8, 7}, {9, 9, 0}, {11, 0, 0}, {9, 9, 3}, {11, 3, 0}, {11, 4, 0},
    {11, 3, 3}, {11, 4, 3}, {11, 5, 3}, {10, 9, 1}, {11, 7, 1}, {10, 9, 4}, {9, 9, 8}, {11, 6, 6},
    {10, 9, 6}, {11, 8, 3}, {10, 9, 7}, {11, 8, 5}, {11, 9, 1}, {12, 0, 0}, {11, 9, 4}, {11, 9, 5},
    {12, 3, 2}, {12, 4, 2}, {12, 5, 2}, {11, 10, 0}, {11, 10, 2}, {12, 6, 4}, {12, 6, 5}, {12, 7, 4},
    {12, 8, 0}, {12, 8, 2}, {11, 10, 7}, {12, 8, 4}, {12, 9, 0}, {11, 11, 2}, {12, 9, 3}, {12, 8, 7},
    {10, 10, 10}, {12, 9, 6}, {12, 8, 8}, {12, 9, 7}, {12, 10, 1}, {13, 2, 2}, {13, 5, 0}, {13, 5, 2},
    {12, 10, 6}, {13, 6, 3}, {13, 6, 4}, {13, 7, 3}, {12, 11, 0}, {12, 11, 2}, {12, 11, 4}, {13, 8, 4},
    {12, 10, 9}, {13, 9, 2}, {13, 9, 4}, {13, 9, 5}, {12, 11, 8}, {13, 8, 8}, {13, 9, 7}, {13, 10, 0},
    {13, 10, 3}, {13, 9, 8}, {13, 10, 5}, {13, 10, 6}, {12, 12, 2}, {12, 12, 4}, {12, 11, 10}, {13, 11, 0},
    {13, 11, 2}, {13, 11, 4}, {13, 11, 5}, {12, 12, 8}, {13, 11, 7}, {12, 11, 11}, {12, 12, 9}, {13, 11, 8},
    {13, 10, 10}, {13, 10, 10}, {13, 11, 9}, {12, 12, 10}, {12, 12, 10}, {14, 0, 0}, {14, 3, 0}, {14, 3, 2},
    {13, 12, 4}, {14, 6, 1}, {14, 7, 0}, {14, 7, 3}, {14, 6, 6}, {14, 8, 1}, {14, 8, 4}, {14, 8, 5},
    {14, 9, 0}, {14, 8, 7}, {14, 9, 5}, {14, 9, 6}, {14, 9, 7}, {13, 12, 10}, {14, 10, 1}, {14, 10, 4},
    {14, 10, 6}, {13, 13, 0}, {13, 13, 4}, {13, 12, 11}, {13, 13, 7}, {14, 11, 0}, {14, 11, 4}, {14, 11, 6},
    {14, 11, 7}, {13, 13, 9}, {14, 11, 8}, {14, 10, 10}, {14, 11, 9}, {13, 13, 10}, {13, 13, 10}, {13, 12, 12},
    {13, 12, 12}, {14, 11, 10}, {14, 11, 10}, {13, 13, 11}, {14, 12, 4}, {14, 12, 6}, {14, 12, 7}, {14, 11, 11},
    {14, 12, 8}, {14, 12, 9}, {14, 12, 9}, {14, 12, 9}, {14, 12, 10}, {14, 12, 10}, {13, 13, 12}, {13, 13, 12},
    {13, 13, 12}, {14, 12, 11}, {14, 12, 11}, {14, 12, 11}, {14, 12, 11}, {14, 13, 0}, {14, 13, 2}, {14, 13, 7},
    {15, 3, 0}, {15, 6, 3}, {15, 7, 6}, {15, 8, 5}, {15, 8, 7}, {15, 8, 8}, {15, 9, 7}, {15, 9, 8},
    {15, 9, 9}, {15, 10, 7}, {15, 10, 8}, {15, 10, 9}, {15, 10, 9}, {15, 11, 0}, {15, 11, 7}, {15, 11, 8},
    {15, 11, 9}, {15, 11, 9}, {14, 13, 12}, {15, 11, 10}, {15, 11, 10}, {15, 11, 10}, {15, 11, 11}, {15, 11, 11},
    {15, 11, 11}, {15, 12, 0}, {15, 12, 7}, {15, 12, 9}, {15, 12, 9}, {15, 12, 10}, {14, 14, 0}, {14, 14, 7},
    {14, 14, 9}, {15, 12, 11}, {14, 14, 10}, {14, 14, 10}, {14, 14, 11}, {14, 14, 11}, {14, 14, 11}, {14, 14, 11},
    {15, 13, 0}, {15, 13, 0}, {15, 13, 9}, {15, 13, 10}, {15, 13, 10}, {14, 14, 12}, {15, 13, 11}, {15, 13, 11},
    {15, 13, 11}, {15, 13, 11}, {15, 13, 11}, {15, 13, 12}, {15, 13, 12}, {15, 13, 12}, {15, 13, 12}, {14, 14, 13},
    {14, 14, 13}, {14, 14, 13}, {14, 14, 13}, {14, 14, 13}, {14, 14, 13}, {15, 13, 13}, {15, 13, 13}, {15, 13, 13},
    {15, 13, 13}, {15, 13, 13}, {15, 13, 13}, {15, 13, 13}, {15, 14, 0}, {15, 14, 0}, {15, 14, 0}, {15, 14, 10},
    {15, 14, 11}, {15, 14, 11}, {15, 14, 11}, {15, 14, 11}, {15, 14, 12}, {15, 14, 12}, {15, 14, 12}, {14, 14, 14},
};
#endif

/* A YM channel's amplitude at each volume, x 10,000: 2^((v - 15) / 2), the
 * curve tools/wav_to_ym4.py fits pairs to. */
static const uint16_t k_ym_amp[16] = {0,    78,   110,  156,  221,  313,
                                      442,  625,  884,  1250, 1768, 2500,
                                      3536, 5000, 7071, 10000};

/* A pair (vA << 4 | vB) as a signed 8-bit sample: the middle of the table
 * entries it is, else the entry nearest in summed amplitude. */
static int8_t s_pair_pcm[256];

static int8_t pcm_of_index(uint32_t index) {
  return (int8_t)((int32_t)(index * 4u + 2u) - 128);
}

static void build_pair_pcm(void) {
  for (uint32_t pair = 0; pair < 256u; pair++) {
    uint32_t a = pair >> 4, b = pair & 15u;
    uint32_t sum = 0, count = 0;
    for (uint32_t i = 0; i < 64u; i++) {
      if (k_ghost_pairs[i][0] == a && k_ghost_pairs[i][1] == b) {
        sum += i;
        count++;
      }
    }
    if (count != 0u) {
      s_pair_pcm[pair] = pcm_of_index((sum + count / 2u) / count);
      continue;
    }
    uint32_t amp = k_ym_amp[a] + k_ym_amp[b];
    uint32_t best = 0, best_diff = UINT32_MAX;
    for (uint32_t i = 0; i < 64u; i++) {
      uint32_t e = k_ym_amp[k_ghost_pairs[i][0]] + k_ym_amp[k_ghost_pairs[i][1]];
      uint32_t diff = e > amp ? e - amp : amp - e;
      if (diff < best_diff) {
        best_diff = diff;
        best = i;
      }
    }
    s_pair_pcm[pair] = pcm_of_index(best);
  }
}

/* --- The source, as a stream of samples ---------------------------------- */

/* YM pairs from a fill callback, a chunk at a time: any size will do. */
#define AUDIO_YM_STAGE_BYTES 256u
static uint8_t s_ym_stage[AUDIO_YM_STAGE_BYTES];
static uint32_t s_ym_stage_pos = AUDIO_YM_STAGE_BYTES;
static int8_t s_pcm_stage[AUDIO_PCM_CHUNK];
static uint32_t s_pcm_stage_pos = AUDIO_PCM_CHUNK;

/* Linear resampling from the source's rate: s0 and s1 are the source
 * samples around the output sample, pos the fraction between them (16.16). */
static bool s_rs_primed;
static int32_t s_rs_s0;
static int32_t s_rs_s1;
static uint32_t s_rs_pos;

static void source_restart(void) {
  s_ym_stage_pos = AUDIO_YM_STAGE_BYTES;
  s_pcm_stage_pos = AUDIO_PCM_CHUNK;
  s_rs_primed = false;
}

static uint32_t source_rate(void) {
  return s_pcm_cb != NULL ? s_pcm_rate : AUDIO_YM_SOURCE_RATE_HZ;
}

/* The next source sample, as signed 8-bit PCM. */
static int32_t source_sample(void) {
  if (s_fill_cb != NULL) {
    if (s_ym_stage_pos >= AUDIO_YM_STAGE_BYTES) {
      s_fill_cb(s_ym_stage, AUDIO_YM_STAGE_BYTES);
      s_ym_stage_pos = 0;
    }
    uint32_t pair = ((s_ym_stage[s_ym_stage_pos] & 15u) << 4) |
                    (s_ym_stage[s_ym_stage_pos + 1u] & 15u);
    s_ym_stage_pos += 2u;
    return s_pair_pcm[pair];
  }
  if (s_pcm_cb != NULL) {
    if (s_pcm_stage_pos >= AUDIO_PCM_CHUNK) {
      s_pcm_cb(s_pcm_stage, AUDIO_PCM_CHUNK);
      s_pcm_stage_pos = 0;
    }
    return s_pcm_stage[s_pcm_stage_pos++];
  }
  return 0;
}

/* The next output sample at `step` source samples per output sample
 * (16.16): 1.0 passes the source through unchanged. */
static int32_t resample(uint32_t step) {
  if (!s_rs_primed) {
    s_rs_s0 = source_sample();
    s_rs_s1 = source_sample();
    s_rs_pos = 0;
    s_rs_primed = true;
  }
  /* At exactly the source's rate, back on the source's samples themselves
   * (a fraction left by another rate would interpolate every one). */
  if (step == 0x10000u) s_rs_pos = 0;
  int32_t v = s_rs_s0 + (((s_rs_s1 - s_rs_s0) * (int32_t)(s_rs_pos >> 4)) >> 12);
  s_rs_pos += step;
  while (s_rs_pos >= 0x10000u) {
    s_rs_pos -= 0x10000u;
    s_rs_s0 = s_rs_s1;
    s_rs_s1 = source_sample();
  }
  return v;
}

static uint32_t rate_step(uint32_t out_rate) {
  return (uint32_t)(((uint64_t)source_rate() << 16) / out_rate);
}

/* `n` samples' YM volumes at the rate the ST achieves: three channels'
 * through audioYmTriples, or a pair (a source at that rate goes through the
 * table unchanged: its pairs are the table's). */
static void produce_ym(uint8_t *dst, uint32_t n) {
  uint32_t step = rate_step(audioYmRateHz);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t value = (uint32_t)(resample(step) + 128);
    if (s_ym_curve) value = s_ym_curve[value];
#if AUDIO_YM_VOICES == 3
    for (uint32_t v = 0; v < 3u; v++) dst[3u * i + v] = audioYmTriples[value][v];
#else
    dst[2u * i] = k_ghost_pairs[value >> 2][0];
    dst[2u * i + 1u] = k_ghost_pairs[value >> 2][1];
#endif
  }
}

/* `n` samples for the DMA chip. */
static void produce_pcm(int8_t *dst, uint32_t n) {
  uint32_t step = rate_step(PROFILE_DMA_RATE_HZ);
  for (uint32_t i = 0; i < n; i++) dst[i] = (int8_t)resample(step);
}

/* One sample into the ring at byte `at` (and into the spill when the spill
 * mirrors it): at 50 fps the six bytes the ST's movep.l and movep.w write to
 * the YM's select and data registers, in the RP's order (cart_shared.h); at
 * 25 fps the pair. */
static inline void ym_put(uint32_t at, const uint8_t *v) {
  uint8_t *p = s_audio_buf + at;
#if PROFILE_YM_MOVEP
  p[0] = v[0];
  p[1] = CART_YM_REG_VOL_A;
  p[2] = v[1];
  p[3] = CART_YM_REG_VOL_B;
  p[4] = v[2];
  p[5] = CART_YM_REG_VOL_C;
#else
  p[0] = v[0];
  p[1] = v[1];
#endif
  if (at < AUDIO_YM_SPILL) {
    uint8_t *q = p + AUDIO_YM_RING;
    for (uint32_t k = 0; k < AUDIO_YM_SAMPLE_BYTES; k++) q[k] = p[k];
  }
}

/* --- The outputs ----------------------------------------------------------- */

static void write_audio_out(void) {
  *((volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_AUDIO_OUT_OFFSET)) =
      (uint16_t)(s_st_silent   ? CART_AUDIO_OUT_NONE
                 : s_prefer_ym ? CART_AUDIO_OUT_YM
                               : CART_AUDIO_OUT_AUTO);
}

/* The output for the ST that just said hello: the ST applies the same rule
 * (userfw.s, .sound_ym). The writer waits while the state starts over. */
static void select_output(uint8_t machine) {
  audio_out_t out =
      (!s_prefer_ym && !s_st_silent && (machine >> 4) == 1u) ? AUDIO_OUT_DMA
                                                             : AUDIO_OUT_YM;
  s_out = AUDIO_OUT_NONE;
  __dmb();
  s_ym_head = s_ym_tail = 0;
  s_dma_head = s_dma_tail = 0;
  s_ym_known = false;
  s_dma_known = false;
  s_last_report = 0xFFFFu;
  memset(s_hold, 0, sizeof s_hold);
  s_dma_hold = 0;
  audioYmRateHz = AUDIO_NATIVE_RATE_HZ;
  s_ym_rate_known = false;
  source_restart();
  /* The ST plays from the buffer before the RP has seen its first report:
   * let that be silence, not the last session's sound: zeros for the DMA
   * chip, silent samples in the ring's format for the YM. */
  if (out == AUDIO_OUT_YM) {
    static const uint8_t silent[AUDIO_YM_VOICES];
    for (uint32_t i = 0; i < AUDIO_YM_RING; i += AUDIO_YM_SAMPLE_BYTES) ym_put(i, silent);
  } else {
    memset(s_audio_buf, 0, CART_AUDIO_BUFFER_SIZE);
  }
  __dmb();
  s_out = out;
  audioOutput = out == AUDIO_OUT_DMA ? 2u : 1u;
  DPRINTF("audio: machine $%02X, %s, %u Hz (%s)\n", (unsigned)machine,
          out == AUDIO_OUT_DMA ? "DMA sound" : "YM",
          (unsigned)(out == AUDIO_OUT_DMA ? PROFILE_DMA_RATE_HZ
                                          : AUDIO_NATIVE_RATE_HZ),
          PROFILE_NAME);
}

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_fill_cb = NULL;
  s_pcm_cb = NULL;
  s_yms_open = false;
  build_pair_pcm();
  write_audio_out();
  s_hellos_seen = st_session_hellos();
  select_output(st_session_machine());

  add_repeating_timer_us(-AUDIO_WRITER_PERIOD_US, audio_writer, NULL,
                         &s_writer_timer);

  DPRINTF("audio_init: %u B at offset $%04X; YM ring %u B, %u a sample, "
          "FIFO %u VBLs\n",
          (unsigned)CART_AUDIO_BUFFER_SIZE, (unsigned)CART_AUDIO_BUFFER_OFFSET,
          (unsigned)AUDIO_YM_RING, (unsigned)AUDIO_YM_SAMPLE_BYTES,
          (unsigned)AUDIO_FIFO_VBLS);
}

void audio_silence_st(bool silent) {
  s_st_silent = silent;
  write_audio_out();
  DPRINTF("audio: the ST %s from its next boot\n",
          silent ? "plays nothing" : "plays");
}

void audio_pause(bool paused) {
  s_paused = paused;
  if (!paused) return;
  /* The whole ring at the level last played: the ST may play past what the
   * writer reaches while the RP's interrupts are off (a flash erase). */
  if (s_out == AUDIO_OUT_DMA) {
    memset(s_audio_buf, (uint8_t)s_dma_hold, CART_AUDIO_DMA_RING_BYTES);
  } else if (s_out == AUDIO_OUT_YM) {
    for (uint32_t at = 0; at < AUDIO_YM_RING; at += AUDIO_YM_SAMPLE_BYTES) ym_put(at, s_hold);
  }
}

void audio_prefer_ym(bool prefer) {
  s_prefer_ym = prefer;
  write_audio_out();
  DPRINTF("audio: %s from the next ST boot\n",
          prefer ? "the YM" : "the DMA chip where there is one");
}

bool audio_uses_dma(void) { return s_out == AUDIO_OUT_DMA; }

void audio_set_ym_curve(const uint8_t curve[256]) { s_ym_curve = curve; }

/* --- Sources ----------------------------------------------------------------- */

void audio_set_fill_callback(audio_fill_cb_t cb) {
  s_pcm_cb = NULL;
  s_fill_cb = cb;
  source_restart();
}

void audio_set_pcm_callback(audio_pcm_cb_t cb, uint32_t rate_hz) {
  s_fill_cb = NULL;
  s_pcm_rate = rate_hz != 0u ? rate_hz : PROFILE_DMA_RATE_HZ;
  s_pcm_cb = cb;
  source_restart();
}

static void audio_loop_cb(uint8_t *buf, uint32_t bytes) {
  uint32_t pos = s_loop_pos;
  const uint32_t total = s_loop_bytes;
  const uint8_t *src = s_loop_data;
  for (uint32_t i = 0; i < bytes; i++) {
    buf[i] = src[pos];
    pos++;
    if (pos >= total) {
      pos = 0;  /* loop */
    }
  }
  s_loop_pos = pos;
}

void audio_play_loop(const uint8_t *data, uint32_t bytes) {
  s_loop_data = data;
  s_loop_bytes = bytes;
  s_loop_pos = 0;
  audio_set_fill_callback(audio_loop_cb);
}

static void audio_pcm_loop_cb(int8_t *buf, uint32_t samples) {
  for (uint32_t i = 0; i < samples; i++) {
    buf[i] = s_pcm_loop_data[s_pcm_loop_pos];
    if (++s_pcm_loop_pos >= s_pcm_loop_samples) s_pcm_loop_pos = 0;
  }
}

void audio_play_pcm_loop(const int8_t *pcm, uint32_t samples, uint32_t rate_hz) {
  if (pcm == NULL || samples == 0u) {
    audio_set_fill_callback(NULL);
    return;
  }
  s_pcm_loop_data = pcm;
  s_pcm_loop_samples = samples;
  s_pcm_loop_pos = 0;
  audio_set_pcm_callback(audio_pcm_loop_cb, rate_hz);
}

static void audio_yms_cb(uint8_t *buf, uint32_t bytes) {
  UINT br = 0;
  FRESULT res = f_read(&s_yms_file, buf, bytes, &br);
  if (res != FR_OK) {
    if (!s_yms_failing) {
      DPRINTF("audio: .YMS read failed (%d), playing silence\n", (int)res);
      s_yms_failing = true;
    }
    /* I/O error -- silence until the next call. The cursor is in
     * an undefined state, so seek back to the data start for the
     * next attempt. */
    for (uint32_t i = 0; i < bytes; i++) {
      buf[i] = 0;
    }
    f_lseek(&s_yms_file, s_yms_data_offset);
    return;
  }
  s_yms_failing = false;
  if (br < bytes) {
    /* EOF mid-fill: wrap to data start and read the remainder. */
    f_lseek(&s_yms_file, s_yms_data_offset);
    UINT br2 = 0;
    f_read(&s_yms_file, buf + br, bytes - br, &br2);
    /* If the file body is shorter than one VBL's worth, pad the
     * still-unfilled tail with silence rather than reading the
     * header bytes. */
    for (uint32_t i = br + br2; i < bytes; i++) {
      buf[i] = 0;
    }
  }
}

int audio_play_yms_file(const char *path) {
  /* Close any previously open YMS file. Idempotent on a fresh init. */
  if (s_yms_open) {
    f_close(&s_yms_file);
    s_yms_open = false;
  }

  FRESULT res = f_open(&s_yms_file, path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("audio_play_yms_file: f_open('%s') failed (%d)\n", path, (int)res);
    return -1;
  }

  uint8_t hdr[AUDIO_YMS_HEADER_SIZE];
  UINT br = 0;
  res = f_read(&s_yms_file, hdr, sizeof(hdr), &br);
  if (res != FR_OK || br != sizeof(hdr)) {
    DPRINTF("audio_play_yms_file: short header read (%d, %u/%u)\n",
            (int)res, (unsigned)br, (unsigned)sizeof(hdr));
    f_close(&s_yms_file);
    return -1;
  }

  if (memcmp(hdr, "YMS1", 4) != 0) {
    DPRINTF("audio_play_yms_file: bad magic %02X%02X%02X%02X\n",
            hdr[0], hdr[1], hdr[2], hdr[3]);
    f_close(&s_yms_file);
    return -1;
  }

  uint32_t rate = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8)
                | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  if (rate != AUDIO_YM_SOURCE_RATE_HZ) {
    DPRINTF("audio_play_yms_file: rate mismatch (file %lu, expected %u)\n",
            (unsigned long)rate, (unsigned)AUDIO_YM_SOURCE_RATE_HZ);
    f_close(&s_yms_file);
    return -1;
  }

  if (hdr[12] != AUDIO_YMS_MODE_DUAL_GHOST) {
    DPRINTF("audio_play_yms_file: unsupported mode tag %u (need %u)\n",
            (unsigned)hdr[12], (unsigned)AUDIO_YMS_MODE_DUAL_GHOST);
    f_close(&s_yms_file);
    return -1;
  }

  s_yms_open = true;
  s_yms_data_offset = AUDIO_YMS_HEADER_SIZE;
  audio_set_fill_callback(audio_yms_cb);

  DPRINTF("audio_play_yms_file: '%s' streaming (rate %lu Hz, mode %u)\n",
          path, (unsigned long)rate, (unsigned)hdr[12]);
  return 0;
}

/* --- The writers (timer interrupt) -------------------------------------------- */

static bool source_set(void) { return s_fill_cb != NULL || s_pcm_cb != NULL; }

/* The latest report in `window`, and whether it is new since the last
 * call (`fresh`). */
typedef enum {
  REPORT_NONE,  /* none in the ring: the ST is not playing */
  REPORT_STALE, /* none new for AUDIO_REPORT_TIMEOUT_US: the ST left userfw */
  REPORT_OK,
} report_t;

static report_t __not_in_flash_func(latest_report)(uint16_t window,
                                                   uint16_t *report,
                                                   bool *fresh) {
  if (!commemul_latest(CART_ROM3_WINDOW_MASK, window, AUDIO_REPORT_LOOKBACK,
                       report)) {
    return REPORT_NONE;
  }
  uint32_t now_us = time_us_32();
  *fresh = *report != s_last_report;
  if (*fresh) {
    s_last_report = *report;
    s_last_report_us = now_us;
  } else if (now_us - s_last_report_us > AUDIO_REPORT_TIMEOUT_US) {
    return REPORT_STALE;
  }
  return REPORT_OK;
}

/* The YM: the ST's cursor runs through the ring and is reported at every
 * VBL. Keep the ring written AUDIO_YM_AHEAD samples ahead of where the
 * cursor is now (the report, plus what the ST read since at the rate it
 * achieves), from the FIFO or, when that is empty, the last pair held. The
 * reports measure that rate: the bytes read between two of them over the
 * time between them, taken over AUDIO_YM_RATE_WINDOW_US and smoothed. */
static void __not_in_flash_func(ym_writer)(void) {
  uint16_t report;
  bool fresh;
  report_t r = latest_report(CART_ROM3_AUDIO_POS_WINDOW, &report, &fresh);
  if (r != REPORT_OK) {
    if (r == REPORT_STALE) s_ym_known = false; /* the ST stopped playing */
    return;
  }
  uint32_t now_us = time_us_32();
  uint32_t play = (report & 0xFFu) * CART_AUDIO_POS_UNIT;
  if (play >= AUDIO_YM_RING) play -= AUDIO_YM_RING; /* never: the ST wraps first */
  /* The ST's cursor is on a sample, within the unit reported: the sample
   * at or before it (six-byte samples do not divide the unit), so every
   * write lands on the ST's samples. */
  play -= play % AUDIO_YM_SAMPLE_BYTES;
  if (!s_ym_known) {
    s_ym_known = true;
    s_ym_front = (play + AUDIO_YM_LEAD * AUDIO_YM_SAMPLE_BYTES) % AUDIO_YM_RING;
    s_ym_prev_play = play;
    s_ym_prev_us = s_last_report_us;
    s_ym_acc_bytes = s_ym_acc_vbls = 0;
  } else if (fresh) {
    audioSlicesWritten++;
    /* The rate the ST achieves. */
    s_ym_acc_bytes += (play + AUDIO_YM_RING - s_ym_prev_play) % AUDIO_YM_RING;
    uint32_t dt_us = s_last_report_us - s_ym_prev_us;
    uint32_t vbls = (dt_us + AUDIO_VBL_PERIOD_US / 2u) / AUDIO_VBL_PERIOD_US;
    s_ym_acc_vbls += vbls != 0u ? vbls : 1u;
    s_ym_prev_play = play;
    s_ym_prev_us = s_last_report_us;
    if (s_ym_acc_vbls >= AUDIO_YM_RATE_VBLS) {
      uint32_t rate = (uint32_t)(((uint64_t)s_ym_acc_bytes / AUDIO_YM_SAMPLE_BYTES) *
                                 1000000u / (s_ym_acc_vbls * AUDIO_VBL_PERIOD_US));
      /* Within reason: a glitch in the reports is not a new rate. The rate
       * only sets the pitch (the writer follows the cursor whatever it is),
       * so once known it moves only for a change of a third of a percent or
       * more, not for each window's rounding: every move is a small step in
       * pitch. And within half a percent of the nominal rate, the nominal
       * rate itself: a YM source then goes through unchanged. */
      if (rate < AUDIO_NATIVE_RATE_HZ * 3u / 4u) rate = AUDIO_NATIVE_RATE_HZ * 3u / 4u;
      if (rate > AUDIO_NATIVE_RATE_HZ * 11u / 10u) rate = AUDIO_NATIVE_RATE_HZ * 11u / 10u;
      uint32_t moved = rate > audioYmRateHz ? rate - audioYmRateHz : audioYmRateHz - rate;
      if (!s_ym_rate_known || moved >= AUDIO_NATIVE_RATE_HZ / 300u) {
        uint32_t smoothed = s_ym_rate_known ? (audioYmRateHz + rate) / 2u : rate;
        s_ym_rate_known = true;
        uint32_t off = smoothed > AUDIO_NATIVE_RATE_HZ ? smoothed - AUDIO_NATIVE_RATE_HZ
                                                       : AUDIO_NATIVE_RATE_HZ - smoothed;
        audioYmRateHz = off < AUDIO_NATIVE_RATE_HZ / 200u ? AUDIO_NATIVE_RATE_HZ : smoothed;
      }
      s_ym_acc_bytes = s_ym_acc_vbls = 0;
    }
    uint32_t ahead = (s_ym_front + AUDIO_YM_RING - play) % AUDIO_YM_RING;
    if (ahead < AUDIO_YM_SAMPLE_BYTES || ahead > AUDIO_YM_RING / 2u) {
      /* The ST read what the RP had not written yet. */
      audioLateSlices++;
      s_ym_front = (play + AUDIO_YM_LEAD * AUDIO_YM_SAMPLE_BYTES) % AUDIO_YM_RING;
    }
  }
  uint32_t since_us = now_us - s_last_report_us;
  if (since_us > AUDIO_YM_EST_MAX_US) since_us = AUDIO_YM_EST_MAX_US;
  uint32_t read_since =
      (uint32_t)((uint64_t)since_us * audioYmRateHz / 1000000u) * AUDIO_YM_SAMPLE_BYTES;
  uint32_t target = (play + read_since + AUDIO_YM_AHEAD * AUDIO_YM_SAMPLE_BYTES) % AUDIO_YM_RING;
  uint32_t n = (target + AUDIO_YM_RING - s_ym_front) % AUDIO_YM_RING;
  if (n > AUDIO_YM_RING / 2u) return; /* already that far */
  bool held = false;
  uint32_t front = s_ym_front;
  uint32_t tail = s_ym_tail;
  uint32_t head = s_ym_head;
  bool paused = s_paused;
  for (uint32_t i = 0; i < n; i += AUDIO_YM_SAMPLE_BYTES) {
    if (tail != head && !paused) {
      const uint8_t *volumes = &s_ym_fifo[(tail % AUDIO_YM_FIFO_SAMPLES) * AUDIO_YM_VOICES];
      for (uint32_t v = 0; v < AUDIO_YM_VOICES; v++) s_hold[v] = volumes[v];
      tail++;
    } else {
      held = true;
    }
    ym_put(front, s_hold);
    front += AUDIO_YM_SAMPLE_BYTES;
    if (front >= AUDIO_YM_RING) front -= AUDIO_YM_RING;
  }
  s_ym_tail = tail;
  s_ym_front = front;
  if (held && !paused && source_set()) audioUnderruns++;
}

/* The DMA chip: at each report the ST has just copied the mirror up to
 * PROFILE_DMA_LEAD ahead of where the chip plays; keep writing it
 * AUDIO_DMA_AHEAD ahead of where it plays now, from the FIFO or, when that
 * is empty, the last sample held. Byte i of the ring is byte i ^ 1 of the
 * buffer. */
static void __not_in_flash_func(dma_writer)(void) {
  uint16_t report;
  bool fresh;
  report_t r = latest_report(CART_ROM3_AUDIO_POS_WINDOW, &report, &fresh);
  if (r != REPORT_OK) {
    if (r == REPORT_STALE) s_dma_known = false;
    return;
  }
  uint32_t play = (report & 0xFFu) * CART_AUDIO_POS_UNIT;
  if (!s_dma_known) {
    s_dma_known = true;
    s_dma_front = (play + PROFILE_DMA_LEAD) & AUDIO_DMA_RING_MASK;
  } else if (fresh) {
    audioSlicesWritten++;
    uint32_t ahead = (s_dma_front - play) & AUDIO_DMA_RING_MASK;
    if (ahead < PROFILE_DMA_LEAD || ahead > CART_AUDIO_DMA_RING_BYTES / 2u) {
      /* The ST copied what the RP had not written yet. */
      audioLateSlices++;
      s_dma_front = (play + PROFILE_DMA_LEAD) & AUDIO_DMA_RING_MASK;
    }
  }
  uint32_t since_us = time_us_32() - s_last_report_us;
  if (since_us > AUDIO_DMA_EST_MAX_US) since_us = AUDIO_DMA_EST_MAX_US;
  uint32_t now_at = play + since_us * PROFILE_DMA_RATE_HZ / 1000000u;
  uint32_t target = (now_at + AUDIO_DMA_AHEAD) & AUDIO_DMA_RING_MASK;
  uint32_t n = (target - s_dma_front) & AUDIO_DMA_RING_MASK;
  if (n > CART_AUDIO_DMA_RING_BYTES / 2u) return; /* already that far */
  bool held = false;
  uint32_t front = s_dma_front;
  uint32_t tail = s_dma_tail;
  uint32_t head = s_dma_head;
  bool paused = s_paused;
  for (uint32_t i = 0; i < n; i++) {
    if (tail != head && !paused) {
      s_dma_hold = s_dma_fifo[tail % AUDIO_DMA_FIFO_BYTES];
      tail++;
    } else {
      held = true;
    }
    s_audio_buf[front ^ 1u] = (uint8_t)s_dma_hold;
    front = (front + 1u) & AUDIO_DMA_RING_MASK;
  }
  s_dma_tail = tail;
  s_dma_front = front;
  if (held && !paused && source_set()) audioUnderruns++;
}

/* Every AUDIO_WRITER_PERIOD_US in a timer interrupt. Everything it touches
 * is in RAM. */
static bool __not_in_flash_func(audio_writer)(repeating_timer_t *timer) {
  (void)timer;
  audio_out_t out = s_out;
  if (out == AUDIO_OUT_YM) {
    ym_writer();
  } else if (out == AUDIO_OUT_DMA) {
    dma_writer();
  }
  return true;
}

/* --- The main loop's part ------------------------------------------------------ */

void audio_render_frame(void) {
  /* A new ST session: the output it plays through. */
  uint32_t hellos = st_session_hellos();
  if (hellos != s_hellos_seen) {
    s_hellos_seen = hellos;
    select_output(st_session_machine());
  }
  if (!source_set()) {
    return;
  }
  if (s_out == AUDIO_OUT_DMA) {
    /* Top up the FIFO a chunk at a time: the writer reads only the samples
     * below s_dma_head. */
    while (AUDIO_DMA_FIFO_BYTES - (s_dma_head - s_dma_tail) >= AUDIO_PCM_CHUNK) {
      int8_t chunk[AUDIO_PCM_CHUNK];
      produce_pcm(chunk, AUDIO_PCM_CHUNK);
      uint32_t head = s_dma_head;
      for (uint32_t i = 0; i < AUDIO_PCM_CHUNK; i++) {
        s_dma_fifo[(head + i) % AUDIO_DMA_FIFO_BYTES] = chunk[i];
      }
      __dmb();
      s_dma_head = head + AUDIO_PCM_CHUNK;
    }
    return;
  }
  /* Top up the FIFO a chunk at a time: the writer reads only the samples
   * below s_ym_head. */
  while (AUDIO_YM_FIFO_SAMPLES - (s_ym_head - s_ym_tail) >= AUDIO_PCM_CHUNK) {
    uint8_t chunk[AUDIO_PCM_CHUNK * AUDIO_YM_VOICES];
    produce_ym(chunk, AUDIO_PCM_CHUNK);
    uint32_t head = s_ym_head;
    for (uint32_t i = 0; i < AUDIO_PCM_CHUNK; i++) {
      uint8_t *volumes = &s_ym_fifo[((head + i) % AUDIO_YM_FIFO_SAMPLES) * AUDIO_YM_VOICES];
      for (uint32_t v = 0; v < AUDIO_YM_VOICES; v++) volumes[v] = chunk[AUDIO_YM_VOICES * i + v];
    }
    __dmb();
    s_ym_head = head + AUDIO_PCM_CHUNK;
  }
}
