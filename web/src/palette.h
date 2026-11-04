/* palette.h for the install gate in a browser or in Node. */
#pragma once
#include <stdint.h>
#define PALETTE_ENTRIES 16
void palette_set(const uint16_t entries[PALETTE_ENTRIES]);
