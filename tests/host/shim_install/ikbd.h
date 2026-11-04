/* Host stand-in for ikbd.h (test_install). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  uint8_t scancode;
  bool is_press;
} ikbd_key_event_t;
#define IKBD_INPUT_KEYBOARD 0
void ikbd_set_input_mode(int mode);
bool ikbd_pop_key(ikbd_key_event_t *key);
void ikbd_pump(void);
void fb_pump_rom3(void);
