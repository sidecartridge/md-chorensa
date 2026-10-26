/**
 * File: bench_logic.h
 * Description: The bench's logic run: the ported game's own code, translated
 *              to C by tools/xlat, played on the RP from a snapshot the host
 *              took after the attract demo's stage load, and timed frame by
 *              frame.
 *
 * Built in only when the build is given a translation (XLAT_RP_DIR, made by
 * tools/xlat/rp_build.py from the user's copy of the game, outside the
 * repository); otherwise the run reports that it has none. Its memory is the
 * snapshot's, in 1 KB pages (xl.h's XL_PAGED accessors): the pages the game
 * writes in RAM borrowed from the framebuffer while the run lasts (the ST
 * shows its last frame), the others read from flash.
 *
 * Every game frame (one XSP_VSYNC) records the game's logic time and the time
 * of its VBL handler; every 60 frames the game's state is compared with what
 * the host recorded at that frame. Started by INSERT on the ST, or over SWD
 * in any build by writing 1 to benchLogicRequest (tools/dev/swd.py logic,
 * which reads the times and the checkpoints back). Blocks the main loop for
 * the run (about a minute); the cartridge keeps serving the ST.
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
  char error[48];
} bench_logic_result_t;

enum {
  BENCH_LOGIC_IDLE,
  BENCH_LOGIC_DONE,
  BENCH_LOGIC_ERROR,
  BENCH_LOGIC_NO_TRANSLATION,
};

/* Whether this build carries a translation. */
bool bench_logic_available(void);
/* Called every bench frame: runs the logic if the host asked for it. */
void bench_logic_poll(void);
/* Run it now (blocks until it is done). */
void bench_logic_run(void);
const bench_logic_result_t *bench_logic_result(void);

#endif /* BENCH_LOGIC_H */
