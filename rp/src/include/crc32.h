#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

/* CRC-32 (IEEE, as zlib's and zip's), in pieces: start from CRC32_START,
 * feed every piece through crc32_update, and the CRC is crc32_end of the
 * result. Plain C, no SDK. */
#define CRC32_START 0xFFFFFFFFu
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t length);
static inline uint32_t crc32_end(uint32_t crc) { return ~crc; }

#endif
