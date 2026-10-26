/**
 * File: bench.h
 * Description: The measurement bench: the screen the firmware shows in place
 *              of the template's demos while the port's parts are measured,
 *              one experiment per key and per host command.
 *
 * The idle screen shows what the ST reported at its hello (machine, TOS,
 * blitter) and what the RP measures (frames published, frames a second, the
 * chunky-to-planar time). ESC keeps ikbd.c's default: back to GEM.
 *
 * Host commands (debug builds, tools/dev/swd.py app NAME [WORD...]):
 *   bench_ping   result: frames published since the ST's boot, plus one
 */

#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>

#include "ikbd.h"

#define DEVHOOKS_APP_BENCH_PING 0x40

void bench_init(void);
void bench_restart(void);
void bench_handle_key(const ikbd_key_event_t *key);
void bench_frame(void);
uint32_t bench_devhook(uint16_t commandId, const uint16_t *payload,
                       uint16_t payloadSize);

#endif  // BENCH_H
