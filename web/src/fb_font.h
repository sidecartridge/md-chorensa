/* fb_font.h for the install gate in a browser or in Node: the font's layout. */
#pragma once
struct FB_FONT {
  int w, h, first_char, num_chars;
  const unsigned char *data;
};
