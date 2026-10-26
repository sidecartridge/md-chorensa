/**
 * File: interp.h
 * Description: The RP2040's interpolator, for the firmware built for the
 *              host (EmuMD, emu/): the subset the firmware uses (lanes 0 and
 *              1 shifted and masked, the full result with base 2, the
 *              accumulators). base[2] is pointer-sized: the firmware keeps a
 *              texture's address in it.
 */
#ifndef EMU_HARDWARE_INTERP_H
#define EMU_HARDWARE_INTERP_H

#include <stdint.h>

typedef struct {
  uint32_t shift;
  uint32_t mask_lsb;
  uint32_t mask_msb;
} interp_config;

typedef struct {
  uint32_t accum[2];
  uintptr_t base[3];
  interp_config lane[2];
} interp_hw_t;

extern interp_hw_t emu_interp0;
#define interp0 (&emu_interp0)

static inline interp_config interp_default_config(void) {
  interp_config c = {0, 0, 31};
  return c;
}

static inline void interp_config_set_shift(interp_config *c, uint32_t shift) {
  c->shift = shift;
}

static inline void interp_config_set_mask(interp_config *c, uint32_t lsb,
                                          uint32_t msb) {
  c->mask_lsb = lsb;
  c->mask_msb = msb;
}

static inline void interp_set_config(interp_hw_t *interp, uint32_t lane,
                                     const interp_config *c) {
  interp->lane[lane] = *c;
}

static inline uint32_t emu_interp_lane(const interp_hw_t *interp,
                                       uint32_t lane) {
  const interp_config *c = &interp->lane[lane];
  uint32_t mask = (uint32_t)(((uint64_t)2 << c->mask_msb) - 1) &
                  ~(((uint32_t)1 << c->mask_lsb) - 1);
  return (interp->accum[lane] >> c->shift) & mask;
}

/* PEEK_FULL: base 2 plus both lanes' shifted and masked accumulators. */
static inline uintptr_t interp_peek_full_result(const interp_hw_t *interp) {
  return interp->base[2] + emu_interp_lane(interp, 0) +
         emu_interp_lane(interp, 1);
}

static inline void interp_add_accumulator(interp_hw_t *interp, uint32_t lane,
                                          uint32_t value) {
  interp->accum[lane] += value;
}

#endif
