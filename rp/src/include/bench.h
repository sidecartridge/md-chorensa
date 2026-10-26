/**
 * File: bench.h
 * Description: The measurement bench: the screen the firmware shows in place
 *              of the template's demos while the port's parts are measured,
 *              one experiment per key and per host command.
 *
 * Its screen is a test card (every line and column, every colour, a bar
 * moving a line a frame) with what the ST reported at its hello (machine,
 * TOS, blitter), what the RP measures (frames published, frames a second,
 * the chunky-to-planar time) and the frames the ST showed with both borders
 * open, one, or none. F1..F4 pick the IKBD's input mode (keys, mouse, mouse
 * and stick 1, sticks). ESC keeps ikbd.c's default: back to GEM.
 *
 * F5 makes the ST play no sound from its next boot, or play again. F6 plays
 * a 440 Hz test tone through the PCM path, or stops it. F7 cycles the copy
 * mode: auto (the blitter on the DMA sound path), the CPU, the blitter; the
 * screen says which layout the RP publishes. F8 cycles the lines a blitter
 * piece: the ST's default, 32, 4, 8.
 *
 * Host commands (debug builds, tools/dev/swd.py app NAME [WORD...]):
 *   bench_ping       result: frames published since the ST's boot, plus one
 *   bench_silent 0|1 the ST plays no sound (1) from its next boot, or plays
 *   bench_tone HZ    a sine of HZ through the PCM path; 0 stops it
 *   bench_copy_mode MODE [LINES]  who copies (0 auto, 1 CPU, 2 blitter) and
 *                    the lines a blitter piece (0: the ST's default)
 *   bench_copy_lines LINES: the lines the ST copies, 256 or 240 (F9 toggles)
 *   F10 on the ST: the YM from the next boot, or the DMA chip again
 *   bench_still 0|1  stop drawing (1) and publish, every frame, what the
 *                    chunky buffer holds with benchStillPalette (written
 *                    over SWD: tools/dev/swd.py still); 0 back to the card
 */

#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>

#include "ikbd.h"

#define DEVHOOKS_APP_BENCH_PING 0x40
#define DEVHOOKS_APP_BENCH_SILENT 0x41
#define DEVHOOKS_APP_BENCH_TONE 0x42
#define DEVHOOKS_APP_BENCH_COPY_MODE 0x43
#define DEVHOOKS_APP_BENCH_COPY_LINES 0x44
#define DEVHOOKS_APP_BENCH_STILL 0x45

void bench_init(void);
void bench_restart(void);
void bench_handle_key(const ikbd_key_event_t *key);
void bench_frame(void);
uint32_t bench_devhook(uint16_t commandId, const uint16_t *payload,
                       uint16_t payloadSize);

#endif  // BENCH_H
