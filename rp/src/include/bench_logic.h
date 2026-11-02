/**
 * File: bench_logic.h
 * Description: The bench's logic run: the ported game's own code, translated
 *              to C by tools/xlat, played on the RP from a snapshot the host
 *              took at the title after the boot, and timed frame by frame.
 *
 * A store build (XLAT_STORE=1) carries no game code: the run copies the
 * images on the SD card (COMMON.IMG and MENUS.IMG in the app's folder, made
 * by tools/xlat/rp_store.py from the user's copy of the game, and each
 * scene's image when the game enters it: STAGE1.IMG..STAGE7.IMG) into the
 * flash store (store.h) unless the store holds them already, timing it, and
 * runs from there. Other builds report that they have no translation. The
 * game's memory is the snapshot's, in pages of 256 bytes (xl.h's XL_PAGED
 * accessors): the pages the game writes in RAM (a pool: the chunky
 * buffer's part a banded frame leaves, an array of the firmware's,
 * xl_rp_ram, and the cartridge window's free arena; xl_store.h), the others
 * read from the stage's image. The ST shows its
 * last frame while the run lasts.
 *
 * Every game frame (one XSP_VSYNC) counts the game's logic time in a histogram
 * (benchLogicHistogram, 32 us a bin) and times its VBL handler; every 60 frames the game's state is compared with what
 * the host recorded at that frame. Started by INSERT on the ST, or over SWD
 * in any build by writing 1 to benchLogicRequest (tools/dev/swd.py logic,
 * which reads the times and the checkpoints back). Blocks the main loop for
 * the run (about ten seconds); the cartridge keeps serving the ST.
 *
 * The host puts the images on the card over SWD (tools/dev/swd.py sd-put),
 * through benchSd: it sets `hold`, waits for `held` (the bench stops drawing,
 * and the chunky framebuffer is the transfer's buffer), then for each chunk
 * writes the bytes, the path, offset and length, and sets `command`; the
 * bench writes them to the card and clears `command`, `status` the bytes
 * written or -1.
 */

#ifndef BENCH_LOGIC_H
#define BENCH_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* The run's result, readable over SWD. Times in microseconds. */
typedef struct {
  uint32_t state;       /* BENCH_LOGIC_* */
  uint32_t first_frame; /* the snapshot's */
  uint32_t frames;      /* game frames played */
  uint32_t logic_sum, logic_max;
  uint32_t vbl_sum, vbl_max;
  uint32_t checkpoints;     /* compared */
  uint32_t mismatches;      /* of them */
  uint32_t first_mismatch;  /* its frame, or 0 */
  uint32_t error_address;   /* where the run stopped on an error */
  uint32_t total_us;
  uint32_t load_status[2];  /* the common and stage images: store.h's STORE_* */
  uint32_t load_us[2];      /* how long each copy took (or its check) */
  uint32_t scene_changes;   /* the slot loaded with another scene, in the run */
  uint32_t scene_kind;      /* the last one's (xl_store.h's kinds) */
  uint32_t scene_frame;     /* the frame it started at */
  uint32_t scene_us;        /* how long it took: the copy and the pages set */
  uint32_t scene_pages;     /* the pages set to their content after the boot */
  char error[48];
} bench_logic_result_t;

#define BENCH_SONG_LOG 24u /* a whole game's 21 songs and the start */
#define BENCH_SCENE_KINDS 9u /* xl_store.h's kinds: 1-7 the stages, 8 the menus */
#define BENCH_SONG_MISSED 0x80u

/* The held frame (benchPlayHoldAt) drawn again, unshown, BENCH_HOLD_DRAWS
 * times whole and without each part: the mean of each, in microseconds,
 * the c2p included (swd.py play --hold prints the parts). */
#define BENCH_HOLD_DRAWS 16u
typedef struct {
  uint32_t frame, sprites;
  uint32_t all_us, no_sprites_us, no_text_us, no_pages_us;
} bench_hold_times_t;

/* The play mode's frames, readable over SWD (tools/dev/swd.py play). */
typedef struct {
  uint32_t frames, laps;       /* shown; runs to the end and back to the snapshot */
  uint32_t game_sum, game_max; /* the game's logic and VBL handler a frame, us */
  uint32_t draw_sum, draw_max; /* the picture on both cores and the palette */
  uint32_t publish_sum, publish_max; /* the c2p and the wait for the ST's copy */
  uint32_t audio_sum, audio_max; /* the sound: the song's and the effect's reads and the mix */
  uint32_t songs, fades;       /* MCDRV's plays and fades while it played */
  uint32_t song_log[BENCH_SONG_LOG]; /* the first plays: frame << 8 | BENCH_SONG_MISSED if the
                                        card had none | the song (the common image's table) */
  uint32_t effects, effects_missed; /* _ADPCMOUT's samples; of them, none in the card's bank */
  /* Frames shown more than BENCH_LATE_US after the one before (a VBL late or
   * more), by scene kind (xl_store.h's: 1-7 the stages, 8 the menus); gaps
   * over a second (a scene change, a hold) left out. */
  uint32_t late[BENCH_SCENE_KINDS];
} bench_play_t;
#define BENCH_LATE_US 30000u

enum {
  BENCH_LOGIC_IDLE,
  BENCH_LOGIC_DONE,
  BENCH_LOGIC_ERROR,
  BENCH_LOGIC_NO_TRANSLATION,
  BENCH_LOGIC_LOADING, /* copying images into the store: nothing may read the
                          flash over SWD (the RP locks up) */
};

/* Whether this build can run the translation. */
bool bench_logic_available(void);
/* Called every bench frame: serves the host's writes to the card, runs the
 * logic if the host asked for it, and plays (benchPlay, 1 from boot in a
 * store build: the attract demo from the title's snapshot, drawn and shown
 * at one game frame a VBL, until the host writes 0). True when it took the
 * screen: the bench sets its palette again. */
bool bench_logic_poll(void);
/* While the host writes to the card: the bench draws nothing. */
bool bench_logic_holds_screen(void);
/* Run it now (blocks until it is done). */
void bench_logic_run(void);
const bench_logic_result_t *bench_logic_result(void);

#endif /* BENCH_LOGIC_H */
