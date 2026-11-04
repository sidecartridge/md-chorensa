#include "crc32.h"

/* Four bits at a time: a 64-byte table instead of zlib's kilobyte. */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t length) {
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
