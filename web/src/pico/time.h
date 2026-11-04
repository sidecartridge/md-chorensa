/* The Pico SDK's clock for the install gate in a browser or in Node: it
 * times the work and paces the (absent) screen. */
#ifndef CRWEB_PICO_TIME_H
#define CRWEB_PICO_TIME_H

#include <emscripten.h>
#include <stdint.h>

static inline uint32_t time_us_32(void) {
  return (uint32_t)(uint64_t)(emscripten_get_now() * 1000.0);
}

#endif
