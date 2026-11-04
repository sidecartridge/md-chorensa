/* Host stand-in for palette.h (test_install). */
#pragma once
#include <stdint.h>
#define PALETTE_ENTRIES 16
void palette_set(const uint16_t entries[PALETTE_ENTRIES]);
