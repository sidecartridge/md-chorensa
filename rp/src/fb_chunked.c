/**
 * File: fb_chunked.c
 * Description: Chunked framebuffer storage + Core 0 / Core 1 dispatch
 *              for the chunky-to-planar conversion.
 *
 * `fb_c2p_half(dst, src, src_end)` lives in fb_chunked_asm.S -- the
 * multiplication-based bit-transpose worker that converts a range of
 * the chunked buffer into a contiguous slice of the planar destination.
 *
 * fb_chunky_to_planar() splits the work across both RP2040 cores:
 * Core 0 handles the top half of the rows, Core 1 the bottom half. Sync is
 * a one-word push/pop on the inter-core FIFO.
 *
 * Core 1 runs a perpetual loop in `fb_c2p_core1_loop` waiting for
 * each frame's dispatch. `fb_chunked_init()` launches it once at
 * boot via the pico-sdk's `multicore_launch_core1`.
 *
 * Output staging: both cores write their natural row-major planar
 * output into `fb_planar_scratch` in RP RAM. Once both halves are
 * complete, fb_chunky_to_planar copies the planar bytes into the
 * caller's `planar` (the cart framebuffer at $FA8000) in the order the
 * m68k's copy reads them: the lines from the last to the first, each as
 * its last two 44-byte chunks and then its first 40 bytes, because
 * FBDRV_INLINE stores with predecrementing MOVEMs (or, for the blitter,
 * the planes as they are). See "Framebuffer layout
 * for the m68k's copy" in cart_shared.h.
 */

#include "fb_chunked.h"

#include <string.h>

#include "cart_shared.h"
#include "pico/multicore.h"

/* Per-file -O3, whatever the CMake build type (RP_CMAKE_BUILD_TYPE can
 * make it MinSizeRel, -Os). The chunky->planar
 * conversion + chunk-reversed memcpy here run on the hot per-frame path
 * and are pure compute; the dual-core handshake uses blocking FIFO calls
 * (real SDK calls with barriers), so -O3 is safe. */
#pragma GCC optimize("O3")

uint8_t fb_chunked_buffer[FB_CHUNKED_SIZE] __attribute__((aligned(4)));

/* Asm worker (fb_chunked_asm.S). Processes pixels in [src, src_end)
 * into the planar layout starting at dst. */
extern void fb_c2p_half(uint16_t *dst,
                        const uint8_t *src,
                        const uint8_t *src_end);

#define FB_C2P_HALF_SRC_BYTES (FB_CHUNKED_SIZE / 2)       /* 32768 */
#define FB_C2P_HALF_DST_WORDS (FB_C2P_HALF_SRC_BYTES / 4) /* 8192 uint16_t */

/* Planar scratch buffer: c2p outputs natural row-major planar bytes
 * here; fb_planar_publish then copies them to the caller's `planar`
 * (the cart FB at $FA8000) in the copier's layout.
 * 32 KB on the RP's 128 KB SRAM, 4-byte aligned for uint32 stores. */
static uint16_t fb_planar_scratch[CART_FRAMEBUFFER_SIZE / sizeof(uint16_t)]
    __attribute__((aligned(4)));

/* Generic Core 1 worker loop (dual-core). Pops a job function
 * pointer + arg off the FIFO, runs it, signals completion. Both the c2p
 * bottom half and the demos' band rendering dispatch through this. The
 * fn/arg travel through the FIFO (not shared memory), so the FIFO's
 * push/pop barriers fully order the handoff.
 *
 * Parked here, Core 1 never touches flash: the loop is in RAM and uses
 * the SDK's inline FIFO calls (the out-of-line ones live in flash). So
 * Core 0 may erase or program flash while Core 1 waits -- the long
 * SELECT press erases the global settings -- as long as no job is
 * running. Jobs are always joined (fb_core1_wait) before the main loop
 * goes on, so Core 1 is parked whenever the main loop runs. */
#ifdef EMUMD
/* The firmware built for the host (EmuMD, emu/): a host's pointers do not fit
 * the 32-bit FIFO, so the job waits in a slot and the FIFO only signals; and
 * its SDK has no inline FIFO calls. */
static fb_core1_job_t fb_core1_slot_job;
static void *fb_core1_slot_arg;
#define multicore_fifo_pop_blocking_inline multicore_fifo_pop_blocking
#define multicore_fifo_push_blocking_inline multicore_fifo_push_blocking
#endif

static void __not_in_flash_func(fb_core1_loop)(void) {
  for (;;) {
#ifdef EMUMD
    (void)multicore_fifo_pop_blocking_inline();
    fb_core1_job_t job = fb_core1_slot_job;
    void *arg = fb_core1_slot_arg;
#else
    fb_core1_job_t job =
        (fb_core1_job_t)(uintptr_t)multicore_fifo_pop_blocking_inline();
    void *arg = (void *)(uintptr_t)multicore_fifo_pop_blocking_inline();
#endif
    job(arg);
    multicore_fifo_push_blocking_inline(0); /* done */
  }
}

void __not_in_flash_func(fb_core1_dispatch)(fb_core1_job_t job, void *arg) {
#ifdef EMUMD
  fb_core1_slot_job = job;
  fb_core1_slot_arg = arg;
  multicore_fifo_push_blocking(0);
#else
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)job);
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)arg);
#endif
}

void __not_in_flash_func(fb_core1_wait)(void) {
  (void)multicore_fifo_pop_blocking(); /* wait for done */
}

/* c2p bottom-half job: arg is the bottom-half planar dst pointer. */
static void __not_in_flash_func(fb_c2p_bottom_job)(void *arg) {
  fb_c2p_half((uint16_t *)arg,
              fb_chunked_buffer + FB_C2P_HALF_SRC_BYTES,
              fb_chunked_buffer + FB_CHUNKED_SIZE);
}

void fb_chunked_init(void) {
  /* multicore_launch_core1 blocks until Core 1 has entered the user
   * function, so by the time fb_init returns, Core 1 is parked in the
   * FIFO pop and ready to service the first frame. */
  multicore_launch_core1(fb_core1_loop);
}

void fb_chunked_clear(uint8_t color) {
  memset(fb_chunked_buffer, color, sizeof(fb_chunked_buffer));
}

void __not_in_flash_func(fb_transpose)(void) {
  /* Dispatch bottom half to Core 1. Both cores write into the
   * scratch buffer (natural row-major layout). */
  fb_core1_dispatch(fb_c2p_bottom_job,
                    fb_planar_scratch + FB_C2P_HALF_DST_WORDS);

  /* Top half: Core 0 runs the worker directly, in parallel with Core 1. */
  fb_c2p_half(fb_planar_scratch,
              fb_chunked_buffer,
              fb_chunked_buffer + FB_C2P_HALF_SRC_BYTES);

  /* Wait for Core 1 to finish its half. The join is a synchronization
   * barrier: pico-sdk's FIFO primitives include DMB/DSB internally, so
   * Core 1's planar writes are guaranteed visible to Core 0 by the
   * time this returns. */
  fb_core1_wait();
}

_Static_assert(CART_FB_LINES * CART_FB_LINE_BYTES == CART_FRAMEBUFFER_SIZE,
               "the framebuffer is CART_FB_LINES lines");
_Static_assert(FB_CHUNKED_W / 2 == CART_FB_LINE_BYTES &&
                   FB_CHUNKED_H == CART_FB_LINES,
               "the chunky buffer is the framebuffer's size");

void __not_in_flash_func(fb_planar_publish)(uint16_t *planar, bool natural) {
  /* Scratch -> cart FB in the order the copier reads it (cart_shared.h,
   * "Framebuffer layout for the m68k's copy"): for the blitter the planes as
   * they are; for the MOVEM copy the lines from the last to the first, each
   * as its two 44-byte chunks from the right, then its first bytes. This is
   * the ONLY cart-FB write -- it must run in the m68k's post-copy slack
   * (gated by fb_publish's wait for the ST's ack), and it's fast so it fits.
   * The slow transpose above runs unsynchronized (RP scratch RAM) and
   * overlaps the m68k's copy. Every piece is a multiple of 4 bytes at a
   * 4-byte aligned offset. */
  if (natural) {
    memcpy(planar, fb_planar_scratch, CART_FRAMEBUFFER_SIZE);
    return;
  }
  /* Word loops, not memcpy: three calls a line for 44, 44 and 40 bytes cost
   * about 350 us a frame against 110 for the loops, out of the ST's slack
   * after its copy. */
  const uint32_t *line = (const uint32_t *)fb_planar_scratch +
                         (CART_FB_LINES - 1u) * (CART_FB_LINE_BYTES / 4u);
  uint32_t *cart = (uint32_t *)planar;
  for (uint32_t y = 0; y < CART_FB_LINES; y++) {
    const uint32_t *a = line + (CART_FB_LINE_TAIL + CART_FB_CHUNK_BYTES) / 4u;
    const uint32_t *b = line + CART_FB_LINE_TAIL / 4u;
    for (uint32_t i = 0; i < CART_FB_CHUNK_BYTES / 4u; i++) *cart++ = a[i];
    for (uint32_t i = 0; i < CART_FB_CHUNK_BYTES / 4u; i++) *cart++ = b[i];
    for (uint32_t i = 0; i < CART_FB_LINE_TAIL / 4u; i++) *cart++ = line[i];
    line -= CART_FB_LINE_BYTES / 4u;
  }
}

void __not_in_flash_func(fb_chunky_to_planar)(uint16_t *planar) {
  /* Convenience wrapper: transpose then publish in one call (no VBL
   * sync). fb.c's fb_publish() calls the two halves separately so the
   * transpose can overlap the m68k blit and only the publish waits. */
  fb_transpose();
  fb_planar_publish(planar, false);
}
