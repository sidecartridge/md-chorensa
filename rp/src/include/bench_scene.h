/**
 * File: bench_scene.h
 * Description: The bench's scene: a frame built to the numbers of the
 *              busiest frames of the game this firmware ports (two background
 *              pages at their parallax, 169 or 268 sprites of 16 x 16, a
 *              line of text), drawn by each strategy in turn and timed.
 *
 * A sweep runs every configuration (background as run lists or a
 * colour-key blit; sprites as 8-bit key blits, 4-bit through a palette
 * table, or run lists; patterns from flash or a RAM cache; one core or both
 * in bands), each at 169 and 268 sprites, for a number of frames each, and
 * keeps the draw time, the chunky-to-planar time and the frame period (mean
 * and max) in benchSceneResults. At the end the ST shows the table until the
 * bench is told otherwise. The assets are synthetic, made at build time by
 * tools/gen_scene.py.
 *
 * Started by HELP on the ST (1,000 frames a run), or over SWD in any build by
 * writing the frames a run to benchSceneRequest (tools/dev/swd.py scene).
 * UNDO goes back to the test card.
 */

#ifndef BENCH_SCENE_H
#define BENCH_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#define BENCH_SCENE_FRAMES_DEFAULT 1000u

/* One run of the sweep, readable over SWD (tools/dev/swd.py scene). Times in
 * microseconds; the frames counted exclude a few of warm-up. */
typedef struct {
  uint32_t config;     /* index into the configurations */
  uint32_t sprites;
  uint32_t frames;
  uint32_t draw_sum, draw_max;
  uint32_t convert_sum, convert_max;
  uint32_t period_sum, period_max;
} bench_scene_result_t;

void bench_scene_init(void);
/* Start a sweep of `frames` a run. */
void bench_scene_start(uint32_t frames);
void bench_scene_stop(void);
/* The scene owns the screen: a sweep or its results. */
bool bench_scene_active(void);
/* Draw and publish one frame of the sweep, or the results. */
void bench_scene_frame(void);
/* Called every frame: starts a sweep the host asked for. */
void bench_scene_poll(void);

#endif /* BENCH_SCENE_H */
