/* Host stand-in for debug.h (test_install): DPRINTF to stdout. */
#pragma once
#include <stdio.h>
#define DPRINTF(...) printf(__VA_ARGS__)
