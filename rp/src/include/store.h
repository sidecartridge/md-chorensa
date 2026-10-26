/**
 * File: store.h
 * Description: The flash store of the translated game (tools/xlat/xl_store.h):
 *              images on the SD card copied into their regions of flash when
 *              the store does not hold them already, and files written to the
 *              card from RAM.
 *
 * A copy compares the file's header with the store's and does nothing when
 * they match; otherwise it erases the region in 64 KB blocks, programs the
 * image from the card through a RAM buffer, the page with the header's
 * magic last, and checks the CRC of what landed. Core 0 only, with core 1
 * parked (no job dispatched); interrupts are off during each erase and
 * program (the cartridge keeps serving the ST from RAM). `progress` runs
 * between flash operations, to keep the bus, the IKBD and SELECT pumped.
 */

#ifndef STORE_H
#define STORE_H

#include <stdint.h>

#include "xl_store.h"

enum {
  STORE_COPIED = 0,  /* the image is in the store now */
  STORE_SKIPPED,     /* the store held it already */
  STORE_NO_FILE,     /* the card has no such file */
  STORE_BAD_FILE,    /* not an image, or too big for its region */
  STORE_READ_ERROR,  /* the card failed mid-copy */
  STORE_BAD_CRC,     /* the store's copy does not match its CRC */
  STORE_OTHER_FIRMWARE, /* made for another firmware build: not copied */
};

typedef void (*store_progress_fn)(void);

/* The header of the image at `base` if its magic and version are valid. */
const xl_store_header_t *store_header(uint32_t base);

/* Copy the image in the card's file `path` into the region at `base`
 * (`max` bytes) if it was made for the firmware `firmware` (a build ID).
 * STORE_COPIED or STORE_SKIPPED, or an error. */
int store_load(const char *path, uint32_t base, uint32_t max, const char *firmware,
               store_progress_fn progress);

/* Write `length` bytes from RAM at `offset` of the card's file `path`
 * (offset 0 creates or truncates it). The bytes written, or -1. */
int store_put(const char *path, uint32_t offset, const uint8_t *data, uint32_t length);

/* CRC-32 (IEEE, as zlib's) of `length` bytes. */
uint32_t store_crc32(const uint8_t *data, uint32_t length);

#endif /* STORE_H */
