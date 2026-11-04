#ifndef INSTALL_H
#define INSTALL_H

#include <stdint.h>

/* The install gate: before the game, the card's sound files made from the
 * Amiga port's repository zip in the app's folder: the effects decoded, the
 * songs resampled for the machine's sound path only (the DMA chip's .ST8 or
 * the YM's .YM8; a card moved to the other kind of machine gets the other
 * set at its first start there). A manifest there (INSTALL_MANIFEST) records
 * what is made; at every start the gate reads it and, when the files the
 * path needs are all there, returns at once without the zip; else it makes
 * only what is missing, showing its progress on the ST. */

#define INSTALL_MANIFEST "INSTALL.TXT"
/* Raised when the converter's output changes: every file is made again. */
#define INSTALL_VERSION 1u

enum {
  INSTALL_READY = 0,       /* everything current: the game may start */
  INSTALL_NO_ZIP = -1,     /* no zip in the folder holds the repository */
  INSTALL_BAD_MEMBER = -2, /* a member is missing or differs from the tested one */
  INSTALL_CARD_ERROR = -3, /* a file could not be written (a full card?) */
  INSTALL_READ_ERROR = -4, /* the card's data came back damaged, again and again */
  INSTALL_NO_PACK = -5,    /* an image to unlock, and no keyed copy of it */
  INSTALL_OLD_PACK = -6,   /* the keyed set is another release's */
  INSTALL_BAD_PACK = -7,   /* a keyed file is damaged (or not one) */
};

/* The sound path the songs are made for (install_gate's `path`). */
enum { INSTALL_FOR_DMA = 0, INSTALL_FOR_YM = 1 };
/* A member read again after its data came back damaged (a corrupted read:
 * in SPI mode the card checks nothing, the zip's CRC-32 does). */
#define INSTALL_TRIES 3

/* The songs' buffers (`song_work`): a render's block, its recent frames,
 * the loop's ends and both outputs' next bytes. */
#define INSTALL_SONG_WORK_BYTES (28u * 1024u)

/* The game's images and START.STA: kept on the card as the store loads
 * them; a release ships them keyed (tools/xlat/keyed.py: COMMON.IMK ..,
 * START.STK), encrypted with a key only the zip's program gives (SHA-256 of
 * its sz2.hex), and the gate unlocks each one missing, or made for another
 * firmware, before the sound. `firmware`: this build's ID. */

/* `folder`: the app's folder on the card; `work`: ZIP_WORK_BYTES for the zip
 * reader and `song_work`: INSTALL_SONG_WORK_BYTES, both aligned to 4 and
 * unused by anything else meanwhile (no allocation: the heap has no room
 * for them); `pump`: called often, to keep the cartridge bus, the IKBD and
 * SELECT served. */
int install_gate(const char *folder, void *work, void *song_work, int path, const char *firmware,
                 void (*pump)(void));
/* What the gate is doing, for the host (tools/dev/swd.py reads it): the
 * output being made, its phase (1 a song, 4 the effects, 5 the images), the
 * bytes inflated and written in that phase, and each output's
 * milliseconds. */
typedef struct {
  uint32_t output, phase;
  uint32_t inflated, written;
  uint32_t bar;     /* the bar's filled pixels, as last drawn */
  int32_t error;    /* the zip reader's last error (zipread.h), 0 none */
  uint32_t retries; /* members read again after damaged data */
  uint32_t phase_ms[6];
  uint32_t output_ms[25];
} install_stats_t;
extern volatile install_stats_t installStats;

/* The screen for a failed gate (a status above), until a key: then Booster.
 * `pump` as install_gate's. */
void install_refuse(int status, const char *folder, void (*pump)(void));

#endif
