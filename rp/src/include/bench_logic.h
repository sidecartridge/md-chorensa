/**
 * File: bench_logic.h
 * Description: The bench's logic run: the ported game's own code, translated
 *              to C by tools/xlat, played on the RP from a snapshot the host
 *              took after the attract demo's stage load, and timed frame by
 *              frame.
 *
 * A store build (XLAT_STORE=1) carries no game code: the run copies the
 * images on the SD card (COMMON.IMG and STAGE1.IMG in the app's folder, made
 * by tools/xlat/rp_store.py from the user's copy of the game) into the flash
 * store (store.h) unless the store holds them already, timing it, and runs
 * from there. Other builds report that they have no translation. The
 * game's memory is the snapshot's, in pages of 256 bytes (xl.h's XL_PAGED
 * accessors): the pages the game writes in a RAM array of the firmware's
 * (xl_rp_ram), the others read from the stage's image. The ST shows its
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
  char error[48];
} bench_logic_result_t;

enum {
  BENCH_LOGIC_IDLE,
  BENCH_LOGIC_DONE,
  BENCH_LOGIC_ERROR,
  BENCH_LOGIC_NO_TRANSLATION,
};

/* Whether this build can run the translation. */
bool bench_logic_available(void);
/* Called every bench frame: runs the logic if the host asked for it, and
 * serves the host's writes to the card. */
void bench_logic_poll(void);
/* While the host writes to the card: the bench draws nothing. */
bool bench_logic_holds_screen(void);
/* Run it now (blocks until it is done). */
void bench_logic_run(void);
const bench_logic_result_t *bench_logic_result(void);

#endif /* BENCH_LOGIC_H */
