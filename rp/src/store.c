/**
 * File: store.c
 * Description: The flash store of the translated game. See store.h.
 */

#include "store.h"

#include <string.h>

#include "debug.h"
#include "ff.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define STORE_BLOCK (64u * 1024u) /* an erase: ~150 ms */
#define STORE_CHUNK 4096u         /* read from the card, then programmed */

_Static_assert(sizeof(xl_store_header_t) == 144u, "the header's size is the host's too");
_Static_assert(STORE_CHUNK % FLASH_PAGE_SIZE == 0u, "chunks are whole pages");

/* Static: FatFs's FIL and the chunk are too big for core 0's 4 KB stack. */
static FIL s_file;
static uint8_t s_chunk[STORE_CHUNK] __attribute__((aligned(4)));
static uint8_t s_first_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

/* The CRC's running state (before the final inversion) over more bytes. */
static uint32_t crc_update(uint32_t crc, const uint8_t *data, uint32_t length) {
  static const uint32_t k_nibbles[16] = {
      0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
      0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
      0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
  for (uint32_t k = 0; k < length; k++) {
    crc ^= data[k];
    crc = (crc >> 4) ^ k_nibbles[crc & 15u];
    crc = (crc >> 4) ^ k_nibbles[crc & 15u];
  }
  return crc;
}

uint32_t store_crc32(const uint8_t *data, uint32_t length) {
  return ~crc_update(0xFFFFFFFFu, data, length);
}

const xl_store_header_t *store_header(uint32_t base) {
  const xl_store_header_t *h = (const xl_store_header_t *)base;
  if (h->magic != XL_STORE_MAGIC || h->version != XL_STORE_VERSION) return NULL;
  return h;
}

static void erase(uint32_t offset, uint32_t size) {
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(offset, size);
  restore_interrupts(ints);
}

static void program(uint32_t offset, const uint8_t *data, uint32_t size) {
  uint32_t ints = save_and_disable_interrupts();
  flash_range_program(offset, data, size);
  restore_interrupts(ints);
}

volatile uint32_t storeForceCopy;

int store_load(const char *path, uint32_t base, uint32_t max, const char *firmware,
               store_progress_fn progress) {
  if (f_open(&s_file, path, FA_READ) != FR_OK) return STORE_NO_FILE;
  UINT got = 0;
  xl_store_header_t header;
  if (f_read(&s_file, &header, sizeof header, &got) != FR_OK || got != sizeof header ||
      header.magic != XL_STORE_MAGIC || header.version != XL_STORE_VERSION ||
      header.size < sizeof header || header.size > max || header.size != f_size(&s_file)) {
    f_close(&s_file);
    return STORE_BAD_FILE;
  }
  if (strncmp(header.firmware, firmware, XL_STORE_FIRMWARE_ID - 1u) != 0) {
    f_close(&s_file);
    return STORE_OTHER_FIRMWARE;
  }
  const xl_store_header_t *held = store_header(base);
  if (!storeForceCopy && held && memcmp(held, &header, sizeof header) == 0) {
    f_close(&s_file);
    return STORE_SKIPPED;
  }

  uint32_t offset = base - XIP_BASE;
  uint32_t span = (header.size + STORE_BLOCK - 1u) / STORE_BLOCK * STORE_BLOCK;
  for (uint32_t at = 0; at < span; at += STORE_BLOCK) {
    erase(offset + at, STORE_BLOCK);
    if (progress) progress();
  }
  /* The image in chunks from its start; the first page, which holds the
   * magic, kept back and programmed last. */
  f_lseek(&s_file, 0);
  for (uint32_t at = 0; at < header.size; at += STORE_CHUNK) {
    uint32_t want = header.size - at < STORE_CHUNK ? header.size - at : STORE_CHUNK;
    if (f_read(&s_file, s_chunk, want, &got) != FR_OK || got != want) {
      f_close(&s_file);
      return STORE_READ_ERROR;
    }
    uint32_t pages = (want + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
    memset(s_chunk + want, 0xFF, pages - want);
    if (at == 0) {
      memcpy(s_first_page, s_chunk, FLASH_PAGE_SIZE);
      if (pages > FLASH_PAGE_SIZE) {
        program(offset + FLASH_PAGE_SIZE, s_chunk + FLASH_PAGE_SIZE, pages - FLASH_PAGE_SIZE);
      }
    } else {
      program(offset + at, s_chunk, pages);
    }
    if (progress) progress();
  }
  f_close(&s_file);
  /* What landed, against the header's CRC: the first page's tail from RAM
   * (it is not programmed yet), the rest from the flash. */
  uint32_t head = header.size < FLASH_PAGE_SIZE ? header.size : FLASH_PAGE_SIZE;
  uint32_t crc = crc_update(0xFFFFFFFFu, s_first_page + sizeof header, head - sizeof header);
  crc = ~crc_update(crc, (const uint8_t *)base + head, header.size - head);
  if (crc != header.crc) return STORE_BAD_CRC;
  program(offset, s_first_page, FLASH_PAGE_SIZE);
  return store_header(base) ? STORE_COPIED : STORE_BAD_CRC;
}

int store_put(const char *path, uint32_t offset, const uint8_t *data, uint32_t length) {
  BYTE mode = offset == 0 ? (FA_WRITE | FA_CREATE_ALWAYS) : (FA_WRITE | FA_OPEN_EXISTING);
  if (f_open(&s_file, path, mode) != FR_OK) return -1;
  UINT wrote = 0;
  FRESULT r = f_lseek(&s_file, offset);
  if (r == FR_OK) r = f_write(&s_file, data, length, &wrote);
  f_close(&s_file);
  return r == FR_OK ? (int)wrote : -1;
}
