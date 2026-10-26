/**
 * File: template_extra.c
 * Description: What this firmware uses of the template's hardware layer and
 *              EmuMD's stand-ins do not provide: commemul_latest() and
 *              commemul_getOverruns() on EmuMD's ROM3 ring, and select_poll()
 *              (SELECT is never pressed in EmuMD).
 */
#include <stdbool.h>
#include <stdint.h>

#include "mdfw.h"

#define HISTORY_SAMPLES 4096u
#define HISTORY_MASK (HISTORY_SAMPLES - 1u)

/* The samples seen so far, copied from the ring without consuming it, so the
 * main loop's commemul_poll() still gets every one. One caller: the audio
 * writer's timer. */
static uint16_t history[HISTORY_SAMPLES];
static uint32_t history_count;
static uint32_t peek_cursor;

bool commemul_latest(uint16_t mask, uint16_t match, uint32_t max_back,
                     uint16_t *sample) {
  uint16_t value;
  while (mdfw_rom3_peek(&peek_cursor, &value)) {
    history[history_count & HISTORY_MASK] = value;
    history_count++;
  }
  uint32_t back_limit =
      history_count < HISTORY_SAMPLES ? history_count : HISTORY_SAMPLES;
  if (max_back < back_limit) back_limit = max_back;
  for (uint32_t back = 1; back <= back_limit; back++) {
    value = history[(history_count - back) & HISTORY_MASK];
    if ((value & mask) == match) {
      *sample = value;
      return true;
    }
  }
  return false;
}

uint32_t commemul_getOverruns(void) { return mdfw_rom3_dropped(); }

void select_poll(void) {}
