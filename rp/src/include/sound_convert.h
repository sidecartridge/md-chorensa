#ifndef SOUND_CONVERT_H
#define SOUND_CONVERT_H

#include <stdint.h>

/* The game's sound for the ST, made from the user's files: the effects
 * decoded from the X68000's ADPCM, and the songs (the Amiga port's renders,
 * .crm) mixed to mono and resampled for the DMA chip and the YM. Plain C, no
 * SDK, in pieces: the web converter's install gate and the host make the same
 * bytes. */

/* --- The effects -------------------------------------------------------- */

/* The MSM6258's ADPCM as MAME's okim6258 decodes it: a 12-bit signal
 * stepped by 49 steps (16 x 1.1^n), each byte's low nibble first, from
 * signal -2 and step 0 at every playback's start; the top 8 bits kept. */
typedef struct {
  int signal, step;
} sc_adpcm_t;

void sc_adpcm_start(sc_adpcm_t *adpcm);
/* `bytes` of ADPCM to 2 x `bytes` signed 8-bit samples. */
void sc_adpcm_decode(sc_adpcm_t *adpcm, const uint8_t *in, uint32_t bytes, int8_t *out);

/* --- The songs ---------------------------------------------------------- */

#define SC_CRM_HEADER 32u   /* a .crm's header ('CRSM', big endian) */
#define SC_SONG_HEADER 32u  /* a .ST8 / .YM8's header ('CRS8', little endian) */
#define SC_CRM_BLOCK_MAX 4096u

typedef struct {
  uint32_t rate;       /* the render's, 22,050 Hz */
  uint32_t intro;      /* frames before the loop */
  uint32_t loop;       /* the loop's frames */
  uint32_t block;      /* frames a block: its left samples, then its right */
  uint16_t fade_unit;  /* 1/10,000 s per unit of MCDRV's fade speed */
  uint16_t level;      /* 1-64, the loudest 64 */
} sc_crm_t;

/* A .crm's header read: 0, or -1 when it is not one this converter knows
 * (stereo, blocks of at most SC_CRM_BLOCK_MAX frames). */
int sc_crm_header(const uint8_t header[SC_CRM_HEADER], sc_crm_t *crm);
/* (A song without a loop, as SZ2_TMP, has loop 0: it ends after its intro.) */

/* A .crm's body (after its header) to its frames in mono, as left + right
 * (twice the level, so that no bit is lost before the resampler halves
 * it), in pieces of any size. */
typedef struct {
  sc_crm_t crm;
  uint32_t frame;   /* the next frame's number */
  uint32_t at;      /* bytes into the current block */
  int8_t left[SC_CRM_BLOCK_MAX];
} sc_unblock_t;

typedef int (*sc_mono_fn)(void *arg, const int16_t *mono, uint32_t frames);
void sc_unblock_start(sc_unblock_t *unblock, const sc_crm_t *crm);
/* 0, or the first non-zero from `out`. */
int sc_unblock(sc_unblock_t *unblock, const uint8_t *body, uint32_t bytes, sc_mono_fn out,
               void *arg);

/* A song at one of the ST's rates: the intro, then the loop as the third
 * of three loops played after the intro, so that it joins the intro's end
 * and its own. The source is the render's mono frames, intro then loop. */
typedef struct {
  uint32_t rate;       /* the output's: RESAMPLE_ST_RATE or RESAMPLE_YM_RATE */
  uint32_t intro;      /* samples of intro */
  uint32_t loop;       /* samples of loop */
  uint32_t loop_from;  /* the loop's first sample in the three loops' stream */
} sc_song_t;

/* The plan for `crm` at `rate`; 0, or -1 for a rate without a filter. */
int sc_song_plan(const sc_crm_t *crm, uint32_t rate, sc_song_t *song);
/* The .ST8 / .YM8 header: 'CRS8', the rate, the intro's and the loop's
 * samples (u32), the fade unit and the level (u16), zeros. */
void sc_song_header(const sc_crm_t *crm, const sc_song_t *song, uint8_t out[SC_SONG_HEADER]);
/* The source frame behind the stream's frame `index`, or -1 past the
 * stream's end (silence): the intro, then the loop three times. */
int32_t sc_song_source(const sc_crm_t *crm, uint32_t index);

/* The resampler, one output sample at a time from a stream it reads in
 * order: `get` gives the stream's frame (left + right, 0 for silence) and is
 * called once per frame; each output is half the filtered sum, rounded. */
typedef int (*sc_frame_fn)(void *ctx, uint32_t index);
typedef struct {
  const int16_t *table;
  uint32_t taps, in_rate, out_rate;
  uint32_t at, remainder;  /* the output's position: frame `at` + remainder / out_rate */
  uint32_t next;           /* the stream's next frame to fetch */
  int16_t window[64];      /* frames next - 64 .. next - 1, by index & 63 */
  sc_frame_fn get;
  void *ctx;
} sc_resampler_t;

/* At output sample `index` of a stream, for one of the song rates. */
int sc_resampler_start(sc_resampler_t *r, uint32_t rate, uint32_t index, sc_frame_fn get,
                       void *ctx);
int8_t sc_resampler_next(sc_resampler_t *r);

#endif
