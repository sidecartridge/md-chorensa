#include "sound_convert.h"

#include <string.h>

#include "resample_table.h"

/* --- The effects -------------------------------------------------------- */

/* 16 x 1.1^n rounded down, n = 0..48. */
static const uint16_t k_steps[49] = {
    16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,  45,  50,  55,  60,  66,  73,
    80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552};
static const int8_t k_shift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

void sc_adpcm_start(sc_adpcm_t *adpcm) {
  adpcm->signal = -2;
  adpcm->step = 0;
}

void sc_adpcm_decode(sc_adpcm_t *adpcm, const uint8_t *in, uint32_t bytes, int8_t *out) {
  int signal = adpcm->signal, step = adpcm->step;
  for (uint32_t k = 0; k < 2u * bytes; k++) {
    int nib = (in[k / 2u] >> ((k & 1u) ? 4 : 0)) & 15;
    int v = k_steps[step];
    int d = v / 8 + ((nib & 4) ? v : 0) + ((nib & 2) ? v / 2 : 0) + ((nib & 1) ? v / 4 : 0);
    signal += (nib & 8) ? -d : d;
    if (signal > 2047) signal = 2047;
    if (signal < -2048) signal = -2048;
    step += k_shift[nib & 7];
    if (step < 0) step = 0;
    if (step > 48) step = 48;
    out[k] = (int8_t)(signal >> 4);
  }
  adpcm->signal = signal;
  adpcm->step = step;
}

/* --- The songs ---------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uint8_t *p, uint32_t v) {
  for (int b = 0; b < 4; b++) p[b] = (uint8_t)(v >> (8 * b));
}
static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

int sc_crm_header(const uint8_t header[SC_CRM_HEADER], sc_crm_t *crm) {
  if (memcmp(header, "CRSM", 4) != 0 || be16(header + 6) != 2) return -1;
  crm->rate = be32(header + 8);
  crm->intro = be32(header + 12);
  crm->loop = be32(header + 16);
  crm->block = be32(header + 20);
  crm->fade_unit = be16(header + 26);
  crm->level = be16(header + 28) ? be16(header + 28) : 64u;
  if (crm->block == 0 || crm->block > SC_CRM_BLOCK_MAX || crm->rate == 0)
    return -1;
  return 0;
}

void sc_unblock_start(sc_unblock_t *unblock, const sc_crm_t *crm) {
  unblock->crm = *crm;
  unblock->frame = 0;
  unblock->at = 0;
}

int sc_unblock(sc_unblock_t *u, const uint8_t *body, uint32_t bytes, sc_mono_fn out, void *arg) {
  uint32_t frames = u->crm.intro + u->crm.loop;
  int16_t mono[256];
  while (bytes && u->frame < frames) {
    uint32_t n = frames - u->frame < u->crm.block ? frames - u->frame : u->crm.block;
    if (u->at < n) { /* the block's left samples */
      uint32_t take = n - u->at < bytes ? n - u->at : bytes;
      memcpy(u->left + u->at, body, take);
      u->at += take;
      body += take;
      bytes -= take;
      continue;
    }
    /* Its right samples, each with its left one. */
    uint32_t k = u->at - n;
    uint32_t take = n - k < bytes ? n - k : bytes;
    if (take > sizeof mono / sizeof *mono) take = sizeof mono / sizeof *mono;
    for (uint32_t j = 0; j < take; j++)
      mono[j] = (int16_t)(u->left[k + j] + (int8_t)body[j]);
    int stop = out(arg, mono, take);
    if (stop) return stop;
    u->at += take;
    body += take;
    bytes -= take;
    if (u->at == 2u * n) {
      u->frame += n;
      u->at = 0;
    }
  }
  return 0;
}

/* `frames` at the render's rate in samples at `rate`, rounded. */
static uint32_t scaled(uint64_t frames, uint32_t rate, uint32_t from) {
  return (uint32_t)((2u * frames * rate + from) / (2u * (uint64_t)from));
}

int sc_song_plan(const sc_crm_t *crm, uint32_t rate, sc_song_t *song) {
  if (rate != RESAMPLE_ST_RATE && rate != RESAMPLE_YM_RATE) return -1;
  song->rate = rate;
  song->intro = scaled(crm->intro, rate, crm->rate);
  song->loop = scaled(crm->loop, rate, crm->rate);
  song->loop_from = scaled((uint64_t)crm->intro + 2u * crm->loop, rate, crm->rate);
  return 0;
}

void sc_song_header(const sc_crm_t *crm, const sc_song_t *song, uint8_t out[SC_SONG_HEADER]) {
  memset(out, 0, SC_SONG_HEADER);
  memcpy(out, "CRS8", 4);
  put32(out + 4, song->rate);
  put32(out + 8, song->intro);
  put32(out + 12, song->loop);
  put16(out + 16, crm->fade_unit);
  put16(out + 18, crm->level);
}

int32_t sc_song_source(const sc_crm_t *crm, uint32_t index) {
  if (index < crm->intro + crm->loop) return (int32_t)index;
  if (crm->loop && (uint64_t)index < (uint64_t)crm->intro + 3u * crm->loop)
    return (int32_t)(crm->intro + (index - crm->intro) % crm->loop);
  return -1;
}

int sc_resampler_start(sc_resampler_t *r, uint32_t rate, uint32_t index, sc_frame_fn get,
                       void *ctx) {
  if (rate == RESAMPLE_ST_RATE) {
    r->table = &k_resample_st[0][0];
    r->taps = RESAMPLE_ST_TAPS;
  } else if (rate == RESAMPLE_YM_RATE) {
    r->table = &k_resample_ym[0][0];
    r->taps = RESAMPLE_YM_TAPS;
  } else {
    return -1;
  }
  r->in_rate = 22050u;
  r->out_rate = rate;
  uint64_t position = (uint64_t)index * r->in_rate;
  r->at = (uint32_t)(position / rate);
  r->remainder = (uint32_t)(position % rate);
  r->get = get;
  r->ctx = ctx;
  /* The window starts at the first tap's frame (or the stream's start). */
  uint32_t lead = r->taps / 2u - 1u;
  r->next = r->at > lead ? r->at - lead : 0;
  for (uint32_t k = 0; k < 64u; k++) r->window[k] = 0;
  return 0;
}

int8_t sc_resampler_next(sc_resampler_t *r) {
  /* The taps cover frames first .. first + taps - 1; those before the
   * stream's start are silence, the others fetched once each, in order. */
  int32_t first = (int32_t)r->at - (int32_t)(r->taps / 2u - 1u);
  int32_t end = first + (int32_t)r->taps;
  while ((int32_t)r->next < end) {
    r->window[r->next & 63u] = (int16_t)r->get(r->ctx, r->next);
    r->next++;
  }
  uint32_t phase = r->remainder * RESAMPLE_PHASES / r->out_rate;
  const int16_t *h = r->table + phase * r->taps;
  int32_t acc = 0;
  for (uint32_t k = 0; k < r->taps; k++) {
    int32_t frame = first + (int32_t)k;
    if (frame >= 0) acc += h[k] * r->window[(uint32_t)frame & 63u];
  }
  int32_t y = (acc + 16384) >> 15;
  if (y > 127) y = 127;
  if (y < -128) y = -128;
  /* The next output's position. */
  r->remainder += r->in_rate;
  while (r->remainder >= r->out_rate) {
    r->remainder -= r->out_rate;
    r->at++;
  }
  return (int8_t)y;
}
