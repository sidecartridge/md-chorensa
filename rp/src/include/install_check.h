#ifndef INSTALL_CHECK_H
#define INSTALL_CHECK_H

#include <stdint.h>

/* What the game needs in the app's folder on the card, made by the web
 * converter (chorensha.sidecartridge.com, which runs install.c's gate) from
 * the Amiga port's repository zip: the game's images, made for one firmware
 * (their headers name it), and the sound: the effects, and each song for
 * the DMA chip (.ST8) and for the YM (.YM8). A manifest (INSTALL_MANIFEST)
 * lists the sound files made, with their sizes. At every start the
 * cartridge checks them (install_ready) and starts the game only when all
 * are there. */

#define INSTALL_MANIFEST "INSTALL.TXT"
#define INSTALL_MANIFEST_HEADER "CHORENSHA INSTALL %u\n"
/* Raised when the converter's output changes: every file is made again. */
#define INSTALL_VERSION 1u
#define INSTALL_EFFECTS_FILE "EFFECTS.SE8"

enum {
  INSTALL_READY = 0,            /* everything current: the game may start */
  INSTALL_MISSING = -8,         /* a file is missing */
  INSTALL_OTHER_FIRMWARE = -9,  /* the files were made for another version */
  INSTALL_INCOMPLETE = -10,     /* a file is there, but not as it was made */
  INSTALL_EMPTY = -11,          /* none of the game's files are there */
  INSTALL_NO_CARD = -12,        /* no card, or one that does not answer */
};

/* The sound path the songs are for. */
enum { INSTALL_FOR_DMA = 0, INSTALL_FOR_YM = 1 };

/* Whether `folder` holds what the game needs on sound path `path`, with the
 * images made for `firmware` (this build's ID): INSTALL_READY, or the first
 * problem found, with its file's name in `what` (16 bytes; empty when no
 * one file is to blame, NULL for none). It stops at the first card error. */
int install_ready(const char *folder, int path, const char *firmware, char *what);

/* For the gate (install.c) too. The outputs: EFFECTS.SE8, then each song's
 * .ST8 and .YM8; the images: COMMON.IMG .. STAGE7.IMG, START.STA. */
#define INSTALL_PATH_BYTES 128u
#define INSTALL_OUTPUTS 25u
#define INSTALL_IMAGES 10u
extern const char *const install_images[INSTALL_IMAGES];
void install_path_of(char *out, const char *folder, const char *name);
void install_output_name(uint32_t k, char *out); /* out: 16 bytes */
/* Whether output k is one sound path `path` uses: the effects, and each
 * song's .ST8 (the DMA chip) or .YM8 (the YM). */
int install_needed(uint32_t k, int path);
uint32_t install_file_size(const char *folder, const char *name); /* 0: none */
/* Whether the manifest is current; `sizes`: each output's size as made, 0
 * for one it does not list. */
int install_read_manifest(const char *folder, uint32_t sizes[INSTALL_OUTPUTS]);
/* Image k: INSTALL_READY when made for `firmware`, else why not
 * (INSTALL_MISSING, INSTALL_INCOMPLETE, INSTALL_OTHER_FIRMWARE,
 * INSTALL_NO_CARD). */
int install_image_state(const char *folder, uint32_t k, const char *firmware);

#endif
