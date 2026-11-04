/**
 * File: cart_shared.h
 * Description: Cart 64 KB shared-region layout + cart-bus helpers.
 *
 * The cart shared region at $FA0000..$FAFFFF on the m68k mirrors RP
 * RAM starting at __rom_in_ram_start__. This header defines the
 * region's sub-block offsets (cart image, command sentinel, dirty
 * frame counter, indexed shared variables, APP_FREE, framebuffer)
 * plus the cart_asM68kLong() helper for exact-value uint32_t RP→
 * m68k writes (see project_cartbus_long_byteswap memory note).
 *
 * The constants used to live in `chandler.h` under the `CHANDLER_*`
 * prefix. The chandler / TPROTOCOL command-channel machinery was
 * removed; the layout constants survived the
 * cull because IKBD and the framebuffer pipeline both need them.
 *
 * Layout must match target/atarist/src/inc/sidecart_layout.s on the m68k
 * side; tests/host/test_layout.py checks that they agree.
 */

#ifndef CART_SHARED_H
#define CART_SHARED_H

#include <inttypes.h>
#include <stdbool.h>

/* All offsets are relative to __rom_in_ram_start__, which mirrors
 * ROM4_ADDR ($FA0000) on the m68k side. Layout (single source of
 * truth, must match target/atarist/src/inc/sidecart_layout.s):
 *
 *   $FA0000  CARTRIDGE             m68k header + code (max 16 KB):
 *                                  main.s (GAME.TOS) and userfw.s,
 *                                  with its unrolled copy inline.
 *   $FA4000  CMD_MAGIC_SENTINEL    4 B  (m68k polls here for
 *                                        NOP / RESET / BOOT_GEM / START)
 *   $FA4004  (reserved)            8 B  (was RANDOM_TOKEN +
 *                                        RANDOM_TOKEN_SEED for the
 *                                        former TPROTOCOL handshake;
 *                                        unused)
 *   $FA400C  FB_FRAME_COUNTER      4 B  (RP-incremented dirty-frame
 *                                        marker; m68k VBL loop skips
 *                                        the cart->ST blit when this
 *                                        is unchanged since the
 *                                        previous iteration)
 *   $FA4010  SHARED_VARIABLES    240 B  (60 indexed 4-byte slots,
 *                                        app-free, but for slots 2..5:
 *                                        commands for the IKBD, slot 6:
 *                                        the audio output, 7: the copy
 *                                        mode, 8: the profile, 9: the
 *                                        lines copied, and slots
 *                                        12..19: the palette).
 *   $FA4100  AUDIO_BUFFER       4096 B (YM volume pairs, or DMA samples)
 *   $FA5100  BOOT_STATUS        2 B  (read as GAME.TOS starts: 0 = start)
 *   $FA5102  BOOT_MESSAGE     126 B  (why the RP refused to start)
 *   $FA5180  APP_FREE          ~11.6 KB free arena, ends at FRAMEBUFFER
 *   $FA8000  FRAMEBUFFER          32 KB (256 lines of 256 px, 4 bpp)
 *   $FAFFFF  end of region
 */
#define CART_CARTRIDGE_CODE_SIZE         0x4000  /* 16 KB cart-image budget */
#define CART_SHARED_BLOCK_OFFSET         CART_CARTRIDGE_CODE_SIZE
#define CART_CMD_SENTINEL_OFFSET         CART_SHARED_BLOCK_OFFSET
#define CART_FB_FRAME_COUNTER_OFFSET     (CART_SHARED_BLOCK_OFFSET + 0x0C)
#define CART_SHARED_VARIABLES_OFFSET     (CART_SHARED_BLOCK_OFFSET + 0x10)
#define CART_SHARED_VARIABLES_SLOTS      60      /* 240 bytes total */

/* Commands for the IKBD, from the RP (ikbd.c): slots 2..5 of
 * SHARED_VARIABLES. A generation word, a length word and up to
 * CART_IKBD_OUT_MAX command bytes, two to a word, the first in the word's
 * high byte (what the m68k reads first). The ST reads the generation every
 * VBL; when it changes, the ST sends the bytes to the IKBD, one per VBL (a 0
 * sends nothing that VBL), then reports the generation's low byte through
 * CART_ROM3_IKBD_OUT_WINDOW. The RP sets CART_IKBD_OUT_BUSY in the
 * generation while it rewrites the block, and clears it with the new
 * generation last. */
#define CART_IKBD_OUT_OFFSET                                                  \
  (CART_SHARED_VARIABLES_OFFSET + (2 * 4))       /* $4018 */
#define CART_IKBD_OUT_SIZE               16
#define CART_IKBD_OUT_MAX                (CART_IKBD_OUT_SIZE - 4)
#define CART_IKBD_OUT_BUSY               0x8000u

/* Which way the ST plays the sound, as the RP asks: a word in slot 6 of
 * SHARED_VARIABLES, read once per ST boot. CART_AUDIO_OUT_AUTO (0, what
 * the window's erase leaves) takes the DMA sound chip on an STE or a Mega
 * STE and the YM elsewhere; CART_AUDIO_OUT_YM keeps to the YM;
 * CART_AUDIO_OUT_NONE plays nothing at all (audio.c). */
#define CART_AUDIO_OUT_OFFSET                                                 \
  (CART_SHARED_VARIABLES_OFFSET + (6 * 4))       /* $4028 */
#define CART_AUDIO_OUT_AUTO              0u
#define CART_AUDIO_OUT_YM                1u
#define CART_AUDIO_OUT_NONE              2u

/* Who copies the cart framebuffer to the ST's screen page, as the RP asks
 * (fb_set_copy_mode()): a word in slot 7 of SHARED_VARIABLES, read every
 * frame (fb_publish() writes it just before the frame counter, so a frame
 * and the layout it was published in agree). The low byte is the mode, the
 * high byte the lines the blitter copies at a time (0: the ST's default,
 * CART_BLIT_PIECE_DEFAULT). CART_BLIT_MODE_AUTO (0, what the window's erase
 * leaves) takes the blitter on the DMA sound path and the CPU elsewhere.
 * Without a blitter the CPU copies. The two copiers read the framebuffer in
 * different layouts: see "Framebuffer layout" below. */
#define CART_BLIT_MODE_OFFSET                                                 \
  (CART_SHARED_VARIABLES_OFFSET + (7 * 4))       /* $402C */
#define CART_BLIT_MODE_AUTO              0u
#define CART_BLIT_MODE_CPU               1u
#define CART_BLIT_MODE_BLITTER           2u
#define CART_BLIT_PIECE_DEFAULT          16u /* lines: 1.0 ms, an IKBD byte waits less than its 1.28 ms */

/* The lines the ST copies of each frame, from its top, as the RP asks
 * (fb_set_copy_lines()): a word in slot 9 of SHARED_VARIABLES, read every
 * frame (fb_publish() writes it just before the frame counter). 0 (what the
 * window's erase leaves) or CART_FB_LINES: all of them; CART_FB_LINES_SHORT:
 * the top 240, the bottom 16 of the frame never shown (that screen's lines
 * stay black), what a plain ST's or a Mega ST's frame holds with sampled
 * sound. Both copiers take it; the frame is published whole either way. */
#define CART_COPY_LINES_OFFSET                                                \
  (CART_SHARED_VARIABLES_OFFSET + (9 * 4))       /* $4034 */
#define CART_FB_LINES_SHORT              240u

/* The app's profile (profile.h), chosen at compile time: how often the ST
 * takes a new frame and how fast it plays the sound. The RP writes it at
 * CART_PROFILE_OFFSET, slot 8 of SHARED_VARIABLES, before the ST boots;
 * userfw reads it once. Per profile: the VBLs a frame; on the YM path
 * Timer-B's count (/4: 614,400 Hz / count), the rate it plays and the bytes
 * of a VBL (two a sample); on the DMA path the chip's mode, its rate, the
 * bytes of a VBL (rounded up) and the lead: the bytes the ST keeps copied
 * ahead of the chip, 3 VBLs, so a missed VBL still finds samples. */
#define CART_PROFILE_OFFSET                                                   \
  (CART_SHARED_VARIABLES_OFFSET + (8 * 4))       /* $4030 */
#define CART_PROFILE_50FPS               0u
#define CART_PROFILE_25FPS               1u
#define CART_PROFILE_50FPS_VBLS          1
#define CART_PROFILE_50FPS_TIMERB_COUNT  79
#define CART_PROFILE_50FPS_YM_RATE_HZ    7777u
/* At 50 fps the ST's copy loop plays the samples itself, one every 5
 * MOVEM pairs, each border handler one, and an idle loop at the same pace
 * between copies: about CART_PROFILE_50FPS_YM_SAMPLES a VBL, the rate
 * above, each written to the ring as six bytes, in the ST's order
 * (register 8, A's volume, register 9, B's volume, register 10, C's
 * volume), which the ST writes to the YM with a movep.l and a movep.w
 * (userfw.s, "Samples by the copy"): the YM's three channels, about 8
 * bits. At 25 fps Timer-D plays two-byte pairs. */
#define CART_PROFILE_50FPS_YM_SAMPLES    155u
#define CART_PROFILE_50FPS_YM_BYTES      (2u * CART_PROFILE_50FPS_YM_SAMPLES)
#define CART_PROFILE_50FPS_YM_SAMPLE_BYTES 6u  /* in the ring: six bytes a sample */
#define CART_YM_REG_VOL_A                8u
#define CART_YM_REG_VOL_B                9u
#define CART_YM_REG_VOL_C                10u
#define CART_PROFILE_50FPS_DMA_MODE      0x82   /* mono, 25,033 Hz */
#define CART_PROFILE_50FPS_DMA_RATE_HZ   25033u
#define CART_PROFILE_50FPS_DMA_BYTES     512u
#define CART_PROFILE_50FPS_DMA_LEAD      1536
#define CART_PROFILE_25FPS_VBLS          2
#define CART_PROFILE_25FPS_TIMERB_COUNT  28
#define CART_PROFILE_25FPS_YM_RATE_HZ    21943u
#define CART_PROFILE_25FPS_YM_BYTES      878u
#define CART_PROFILE_25FPS_YM_SAMPLES    439u
#define CART_PROFILE_25FPS_YM_SAMPLE_BYTES 2u  /* in the ring: the pair as it is */
#define CART_PROFILE_25FPS_DMA_MODE      0x82   /* mono, 25,033 Hz */
#define CART_PROFILE_25FPS_DMA_RATE_HZ   25033u
#define CART_PROFILE_25FPS_DMA_BYTES     512u
#define CART_PROFILE_25FPS_DMA_LEAD      1536

/* 16-entry ST palette published by the RP, applied by the m68k VBL
 * handler to $FFFF8240..$FFFF825E each frame. Format: 16 contiguous
 * 16-bit words. Each word is the standard ST 9-bit palette format
 * 0000.0RRR.0GGG.0BBB. uint16_t writes are transparent across the
 * cart-bus byte-swap so the RP can write the m68k-observable word
 * value directly.
 *
 * Lives inside SHARED_VARIABLES (slots 12..19 = offsets +0x30..0x4F
 * = absolute $FA4040..$FA405F). Apps that don't want RP-driven
 * palette publishing can leave the slot at zeros (= black palette
 * = all-black screen) and write $FFFF8240 from their own m68k code
 * instead. */
#define CART_PALETTE_OFFSET                                                   \
  (CART_SHARED_VARIABLES_OFFSET + (12 * 4))      /* $4040 */
#define CART_PALETTE_ENTRIES             16
#define CART_PALETTE_SIZE                (CART_PALETTE_ENTRIES * 2)  /* 32 B */

/* Audio sample buffer, used one of two ways (audio.c):
 *   - the YM: a ring of CART_AUDIO_YM_RING_BYTES of samples the ST's cursor
 *     (A0) runs through: at 50 fps six bytes a sample (A's volume, 8, B's
 *     volume, 9, C's volume, 10, in the RP's order: the ST writes them to
 *     the YM's select and data registers with a movep.l and a movep.w), at
 *     25 fps the (vA, vB) pair.
 *     The cursor runs on past the ring's end, into the spill
 *     (CART_AUDIO_YM_SPILL_BYTES, which the RP keeps a mirror of the ring's
 *     start: a VBL's samples at most), and the ST's VBL handler brings it
 *     back by the ring's size and reports it (CART_ROM3_AUDIO_POS_WINDOW,
 *     by CART_AUDIO_POS_UNIT bytes). The RP writes ahead of it, and
 *     resamples the app's sound to the rate the ST achieves, which the
 *     reports measure: no sample is skipped or held, whatever interrupts
 *     fall in a VBL;
 *   - the DMA sound chip of an STE or a Mega STE: the whole buffer mirrors
 *     the ring of 8-bit signed samples the chip plays from ST RAM at the
 *     profile's rate. Every VBL the ST reports where the chip plays
 *     (the same window and unit) and copies the mirror up to the profile's
 *     lead ahead of it; the RP writes further ahead. Byte i of the ring is
 *     byte i ^ 1 of the RP's buffer (the cart bus swaps the bytes of each
 *     word). */
#define CART_AUDIO_BUFFER_OFFSET                                              \
  (CART_SHARED_VARIABLES_OFFSET + (CART_SHARED_VARIABLES_SLOTS * 4))
#define CART_AUDIO_BUFFER_SIZE           4096
#define CART_AUDIO_DMA_RING_BYTES        CART_AUDIO_BUFFER_SIZE
#define CART_AUDIO_POS_UNIT              16
#define CART_AUDIO_YM_RING_BYTES         3066  /* 3.3 VBLs at 50 fps, 3.5 at 25 */
#define CART_AUDIO_YM_SPILL_BYTES        1026  /* over a VBL's samples in both profiles, whole
                                                  samples of either (the buffer's last 4
                                                  bytes unused) */

/* Boot block, after the audio buffer. The ST reads the status word each
 * time GAME.TOS starts, before it starts userfw: CART_BOOT_OK (0, which
 * the window's erase at boot leaves) starts the app; anything else makes
 * GAME.TOS print the NUL-terminated text at CART_BOOT_MESSAGE_OFFSET and
 * end, back to the desktop. Written through st_session_veto_boot(). */
#define CART_BOOT_STATUS_OFFSET                                               \
  (CART_AUDIO_BUFFER_OFFSET + CART_AUDIO_BUFFER_SIZE)
#define CART_BOOT_OK                  0u
#define CART_BOOT_VETOED              1u
#define CART_BOOT_MESSAGE_OFFSET      (CART_BOOT_STATUS_OFFSET + 2)
#define CART_BOOT_MESSAGE_SIZE        126  /* bytes, the NUL included */

/* APP_FREE arena starts after the boot block. */
#define CART_APP_FREE_OFFSET                                                  \
  (CART_BOOT_MESSAGE_OFFSET + CART_BOOT_MESSAGE_SIZE)

/* The framebuffer: 256 lines of 256 pixels, ST low-resolution planes (4
 * bitplanes, 128 bytes a line), flush against the top of the 64 KB region:
 * $FA8000..$FAFFFF. APP_FREE's upper bound is the framebuffer's base. */
#define CART_FRAMEBUFFER_SIZE         32768
#define CART_FRAMEBUFFER_OFFSET       (0x10000 - CART_FRAMEBUFFER_SIZE)
#define CART_REGION_END               0x10000  /* 64 KB shared region top */

/* ---------------------------------------------------------------------
 * Framebuffer layout for the m68k's copy
 *
 * The ST shows the 256 lines in an overscan screen (both borders opened,
 * 160 bytes a line in its RAM), the image in the first 128 bytes of each
 * line. userfw's FBDRV_INLINE copies the cartridge framebuffer into the
 * hidden screen page from its last line to its first, three predecrementing
 * MOVEMs a line (A0, A4 and A7 are not in their lists: A0 is the audio
 * cursor, A4 the YM's address and A7 the SP, so samples can be written and
 * interrupts fire during the copy):
 *
 *     movem.l (a6)+, d0-d7/a1-a3        ; 44 bytes
 *     movem.l d0-d7/a1-a3, -(a5)        ;   to line bytes 84..127
 *     movem.l (a6)+, d0-d7/a1-a3        ; 44 bytes
 *     movem.l d0-d7/a1-a3, -(a5)        ;   to line bytes 40..83
 *     movem.l (a6)+, d0-d7/a1-a2        ; 40 bytes
 *     movem.l d0-d7/a1-a2, -(a5)        ;   to line bytes 0..39
 *     lea     -32(a5), a5               ; the line above, at its byte 128
 *
 * The predecrement store is 4 cycles faster than a d16(a5) one (8+8n
 * against 12+8n). So the cartridge framebuffer holds the lines from the
 * last (255) to the first (0), each as its bytes 84..127, then 40..83,
 * then 0..39, in natural order within each piece:
 *
 *     cart bytes       0 ..   43   <-- line 255, bytes 84..127
 *     cart bytes      44 ..   87   <-- line 255, bytes 40..83
 *     cart bytes      88 ..  127   <-- line 255, bytes  0..39
 *     cart bytes     128 ..  171   <-- line 254, bytes 84..127
 *     ...
 *     cart bytes   32728 .. 32767  <-- line 0, bytes 0..39
 *
 * The RP's chunky-to-planar conversion writes natural row-major planes to
 * RP RAM and fb_planar_publish() copies them here in this order.
 *
 * The blitter (an STE, a Mega STE, a Mega ST fitted with one), when it
 * copies instead, reads the lines in natural order, line 0 first and each
 * line's 128 bytes forward, and writes each to its screen line, its Y
 * increment (CART_FB_BLITTER_DST_Y_INC) skipping the 32 bytes right of the
 * image. For it the RP publishes the planes as they are: the natural
 * layout. Which layout a frame is in follows from who copies it (the copy
 * mode word above, the ST's blitter, the sound path), by the same rule on
 * both sides (fb.c, userfw.s).
 */
#define CART_FB_BLITTER_DST_Y_INC     (160 - CART_FB_LINE_BYTES + 2)
#define CART_FB_LINES                 256  /* must match FB_COPY_LINES in target/atarist/src/inc/sidecart_layout.s */
#define CART_FB_LINE_BYTES            128  /* 256 px * 4 bpp / 8 */
#define CART_FB_CHUNK_BYTES           44   /* one MOVEM of 11 longwords: D0-D7, A1-A3 */
#define CART_FB_CHUNKS_A_LINE         2
#define CART_FB_LINE_TAIL             (CART_FB_LINE_BYTES - CART_FB_CHUNKS_A_LINE * CART_FB_CHUNK_BYTES)  /* 40: one MOVEM of D0-D7, A1-A2 */

/* RP→m68k command sentinel values. The m68k polls the longword at
 * CART_CMD_SENTINEL_OFFSET; non-zero values steer it out of the
 * userfw loop or the bootstrap dispatcher. Must match the m68k-side
 * equs in target/atarist/src/inc/sidecart_layout.s. */
#define CART_CMD_NOP        0u
#define CART_CMD_RESET      1u
#define CART_CMD_BOOT_GEM   2u
#define CART_CMD_START      4u

/* ROM3 signalling windows. The ST cannot write the cartridge window, so it
 * tells the RP things by reading ROM3 addresses ($FB0000-$FBFFFF), which
 * commemul's ring captures: the high byte of the captured address says what
 * the read means, the low byte carries a value. Must match the equs in
 * target/atarist/src/inc/sidecart_layout.s.
 *
 *   $FB82xx  an IKBD byte (ikbd.c, IKBD_WINDOW_LO16)
 *   $FB83xx  VBL: the ST has read xx IKBD bytes so far, mod 256 (ikbd.c)
 *   $FB84xx  blit done: the cart framebuffer is free (fb.c)
 *   $FB8500  the keyboard ACIA overran: IKBD bytes were lost (ikbd.c)
 *   $FB87xx  the ST sent the IKBD commands of generation xx (low byte)
 *   $FB88xx  hello: a new ST session starts; xx is the machine (st_session.h)
 *   $FB89xx  TOS version, high byte; sent just before the hello
 *   $FB8Axx  TOS version, low byte; sent just before the hello
 *   $FB8Bxx  the stopwatch (userfw.s TIME_STUDY): point xx of the ST's loop,
 *   $FB8Cxx  then its ticks, high byte, and
 *   $FB8Fxx  low byte (fb.c)
 *   $FB8Dxx  VBL: where the ST plays, by CART_AUDIO_POS_UNIT bytes: the YM
 *            path's cursor in the cart ring, the DMA chip's position in
 *            its ring (audio.c)
 *   $FB8Exx  the ST's features (CART_ST_FEATURE_*), after the hello
 *   $FB90xx  VBL: which borders the frame before had open (CART_BORDER_*,
 *            fb.c) */
#define CART_ROM3_WINDOW_MASK        0xFF00u
#define CART_ROM3_IKBD_COUNT_WINDOW  0x8300u
#define CART_ROM3_BLIT_DONE_WINDOW   0x8400u
#define CART_ROM3_IKBD_OVERRUN_WINDOW 0x8500u
#define CART_ROM3_IKBD_OUT_WINDOW    0x8700u
#define CART_ROM3_HELLO_WINDOW       0x8800u
#define CART_ROM3_TOS_HI_WINDOW      0x8900u
#define CART_ROM3_TOS_LO_WINDOW      0x8A00u
#define CART_ROM3_STUDY_POINT_WINDOW 0x8B00u
#define CART_ROM3_STUDY_HI_WINDOW    0x8C00u
#define CART_ROM3_STUDY_LO_WINDOW    0x8F00u
#define CART_ROM3_AUDIO_POS_WINDOW   0x8D00u
#define CART_ROM3_ST_FEATURES_WINDOW 0x8E00u
#define CART_ST_FEATURE_BLITTER      0x01u
#define CART_ROM3_BORDER_WINDOW      0x9000u
#define CART_BORDER_TOP              0x01u
#define CART_BORDER_BOTTOM           0x02u

/* The cart bus byte-swaps WITHIN each 16-bit word: RP stores LE,
 * m68k reads BE, and the swap makes that transparent for uint16_t.
 * For uint32_t, m68k's BE long-read is two word reads in (high, low)
 * order, but the two 16-bit halves stay in their RP-LE positions --
 * so m68k sees the halves SWAPPED.
 *
 * For exact-value RP→m68k longword protocols (CMD_MAGIC_SENTINEL,
 * etc.) the RP must store the half-swapped value so the m68k's
 * move.l observes the intended uint32_t. Protocols that only care
 * about inequality (the FB dirty-frame counter is the canonical
 * example) don't need this -- both sides see distinct values for
 * distinct writes regardless of the swap. */
static inline uint32_t cart_asM68kLong(uint32_t v) {
  return (v << 16) | (v >> 16);
}

#endif /* CART_SHARED_H */
