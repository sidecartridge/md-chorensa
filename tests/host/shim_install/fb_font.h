/* Host stand-in for fb_font.h (test_install): the font's layout, as the
 * firmware's (the gate draws its glyphs itself). */
#pragma once
struct FB_FONT {
  int w;
  int h;
  int first_char;
  int num_chars;
  const unsigned char *data;
};
