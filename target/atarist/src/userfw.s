; userfw.s -- user firmware module.
;
; Entry point: USERFW ($FA0800), reached from main.s either directly
; (early-boot fast path) or via the rom_function dispatcher when the
; RP issues CMD_START.
;
; Pipeline (per VBL):
;   1. Custom VBL handler at $70 (installed once at boot) wakes the
;      main loop by clearing a flag in ST RAM. We DON'T use XBIOS
;      Vsync (trap #14, #37) -- that trips through TOS's GEMDOS-aware
;      dispatch and adds latency / jitter.
;   2. Read the low word of FB_FRAME_COUNTER_ADDR ($FA400C), which the RP
;      bumps as the last write of every publish. If it has not moved
;      since the last blit (UFW_LAST_FRAME), there is nothing new: skip
;      the blit, the flip and the ack (see FB_FRAME_COUNTER_ADDR).
;   3. Copy the 32 KB cart framebuffer ($FA8000: 256 lines of 256 px)
;      into the hidden ST screen page, an overscan page whose top and
;      bottom borders are open (see "Overscan" below). The copy is a pure
;      68000 MOVEM burst expanded inline via FBDRV_INLINE.
;   4. Flip the video base to the screen we just wrote.
;   5. Toggle A4 between SCREEN_A and SCREEN_B for the next frame.
;   6. Poll CMD_MAGIC_SENTINEL_ADDR ($FA4000). The RP-side IKBD demux
;      writes CMD_BOOT_GEM there when it decodes an ESC press. On
;      match, restore vectors / MFP / VBL / screen base and rts back
;      to the cartridge dispatcher.
;
; IRQ ownership: TOS's HBL ($68) and Timer-A ($134) handlers are stubbed to
; single-rte dummies and their MFP IERA/IERB bits are cleared so they cannot
; fire (Timer-A is the stopwatch in TIME_STUDY builds). The custom VBL
; handler at $70, Timer-B ($120, the bottom border), Timer-C ($114, the top
; border), Timer-D ($110, YM samples) and the keyboard ACIA ($118) are
; userfw's.
;
; IKBD bytes are forwarded by the keyboard ACIA's receive interrupt
; (userfw_acia_irq, MFP GPIP4): each byte as it arrives, emitted via a
; cart-bus read at IKBD_WINDOW_BASE + byte ($FB8200..$FB82FF). The RP
; captures the read via the commemul PIO+DMA ring (no per-read CPU
; overhead) and runs the IKBD demux from its main loop. The RP also
; decides what the IKBD reports (its input modes): userfw_vbl_loop sends the IKBD
; the commands the RP puts at IKBD_OUT_ADDR.
;
; --- Constants ----------------------------------------------------

; The cartridge window, the command values and the ROM3 signalling windows
; (FB_FRAME_COUNTER_ADDR, CMD_MAGIC_SENTINEL_ADDR, PALETTE_ADDR,
; AUDIO_BUFFER_ADDR, FRAMEBUFFER_ADDR, FB_COPY_LINES, IKBD_WINDOW_BASE,
; VBLSYNC_ADDR...), shared with main.s and checked against the RP's
; cart_shared.h.
	include inc/sidecart_layout.s

; Atari ST shifter video base registers (68000-compatible, present
; on every ST/STE/MegaSTE/TT/Falcon). Only HIGH+MID are written;
; the STE-only LOW byte at $FFFF820D stays at TOS's default of 0,
; which matches our 256-byte-aligned screen pages (UFW_SCREEN_A/B).
VIDEO_BASE_ADDR_HIGH  equ $FFFF8201
VIDEO_BASE_ADDR_MID   equ $FFFF8203
; The video counter's middle and low bytes (read with one movep.w from
; VIDEO_REGS + 7), and the 50/60 Hz sync mode: 2 is 50 Hz, 0 is 60 Hz.
VIDEO_REGS            equ $FFFF8200
VIDEO_SYNC            equ $FFFF820A
SYNC_50HZ             equ 2
SYNC_60HZ             equ 0

; Palette index 0 doubles as the border colour. We poke it at three
; points in the VBL loop so the ST border visualises blit timing
; (cherry-picked from md-sprites-demo). Foreground text in the FB also
; uses idx 0, so during BLIT_MARK_RUNNING the white text momentarily
; becomes black -- harmless since the blit is only a few ms long.
PALETTE_BASE          equ $FFFF8240          ; 16 hardware palette words ($FFFF8240..$FFFF825E)
PALETTE_IDX0          equ PALETTE_BASE
BLIT_MARK_VSYNC       equ $000               ; black: vsync returned, copy not yet started
BLIT_MARK_RUNNING     equ $777               ; white: cart->ST copy in flight
BLIT_MARK_DONE        equ $070               ; green: FBDRV_INLINE returned

; FBDRV_DEBUG_MARKS = 1 paints palette-idx-0 with the three border
; band colours above (black/white/green) at vsync / blit-running /
; blit-done. Useful for timing measurement on a CRT but flickers
; any visible content drawn in palette idx 0 (incl. the cart-side
; palette publish below) -- keep it 0 for the demos. Set to
; 1 when measuring.
FBDRV_DEBUG_MARKS     equ 0

; TIME_STUDY = 1 turns MFP Timer-A into a free-running stopwatch: /10,
; 4.07 us a tick, its wraps (every 256 ticks) counted by an interrupt
; (userfw_timera_tick, about 1% of the CPU). The VBL and points of the loop
; (the wake, the copy's start and end, the loop's end) report the
; stopwatch's low 16 bits (STUDY_POINT, three ROM3 reads), so every duration
; is a difference on one timeline, even across a VBL. While the YM plays at
; 50 fps the VBL also reports how many samples the last VBL wrote (the
; cursor's distance from where it was at the VBL before: STUDY_POINT_SAMPLES,
; the count in place of the ticks), and the top border handler the Timer-C count it switched
; at (STUDY_POINT_TOPWAKE). Debug builds of the RP keep the points and the
; slack before the VBL (fb.c; tools/dev/swd.py stopwatch). About 40 us a
; VBL: measuring builds only. The wrap interrupt must run within 1.04 ms of the wrap: a
; hold of the CPU longer than that (a blitter piece over 16 lines) loses a
; wrap, and the copy reads shorter than it is.
TIME_STUDY            equ 0

; The profile this image is assembled for (PROFILE_25FPS_BUILD, from the
; Makefile: the build passes the RP's APP_PROFILE). At 50 fps the YM path's
; samples are written by the copy, the border handlers and an idle loop
; (FB_INLINE_SAMPLES, see "Samples by the copy"); at 25 fps by Timer-D.
    ifnd    PROFILE_25FPS_BUILD
PROFILE_25FPS_BUILD   equ 0
    endc
FB_INLINE_SAMPLES     equ (1 - PROFILE_25FPS_BUILD)
MFP_TADR              equ $FFFFFA1F          ; Timer-A data: the count while it runs
TIMERA_STUDY_DIV10    equ 2                  ; Timer-A control: delay mode, /10
STUDY_POINT_WAKE      equ 0
STUDY_POINT_COPY      equ 1
STUDY_POINT_COPIED    equ 2
STUDY_POINT_IDLE      equ 3
STUDY_POINT_VBL       equ 4
STUDY_POINT_SAMPLES   equ 5                  ; the samples the last VBL wrote
STUDY_POINT_TOPWAKE   equ 6                  ; Timer-C's ticks at the top switch
STUDY_POINT_BOTWAKE   equ 7                  ; the counter's bytes into line 262 at the bottom switch (or the miss)

; STUDY_POINT n: tells the RP the loop reached point n, with the stopwatch's
; ticks (TIME_STUDY). Clobbers D0-D2 and A1.
STUDY_POINT macro
    ifne    TIME_STUDY
    moveq   #\1, d2
    bsr     study_report
    endc
    endm

; Per-VBL state area, above the screen pages and the DMA ring: the
; current-page pointer UFW_SCREEN_PAGE, the saved TOS VBL vector / Physbase
; result and the words below.
; UFW_RESET_STUB: .cold_reset copies userfw_reset_stub here and runs it,
; so the ST's last instructions before its cold reset come from RAM.
UFW_RESET_STUB        equ $00077F00          ; up to $77F7F
; The machine from the hello (family in bits 7..4), whether the DMA sound
; chip plays (-1) or Timer-B and the YM (0), and where the last copy into
; the DMA ring ended (see AUDIO_BUFFER_ADDR, UFW_DMA_RING).
UFW_MACHINE           equ $00077F80          ; word
UFW_AUDIO_DMA         equ $00077F82          ; word
UFW_DMA_FRONT         equ $00077F84          ; word: an offset in the ring
; A blitter found at boot (XBIOS Blitmode): 1, else 0 (see BLIT_MODE_ADDR).
UFW_HAS_BLITTER       equ $00077F86          ; word
; The profile read at boot (PROFILE_ADDR): the DMA chip's lead, the VBLs a
; frame, Timer-B's count, the DMA chip's mode. The VBLs counted by userfw_vbl,
; and the count when the last frame was taken.
UFW_DMA_LEAD          equ $00077F88          ; word
UFW_VBLS_A_FRAME      equ $00077F8A          ; word
UFW_TIMERB_COUNT      equ $00077F8C          ; word
UFW_DMA_MODE          equ $00077F8E          ; word
UFW_VBL_COUNT         equ $00077F90          ; word
UFW_FRAME_VBL         equ $00077F92          ; word
; The stopwatch's wraps (TIME_STUDY).
UFW_SW_WRAPS          equ $00077F94          ; word
; The borders (see "Overscan"): those opened this frame (BORDER_TOP,
; BORDER_BOTTOM, reported by userfw_vbl); -1 when the last bottom border
; started Timer-C for this frame's top one; the low word of the page shown
; this frame; and the video counter's distance from it at the start of line
; 262 (the last line of a normal screen) the bottom handler waits for.
UFW_BORDERS           equ $00077F96          ; word
UFW_TOP_ARMED         equ $00077F98          ; word
UFW_SHOWN_BASE        equ $00077F9A          ; word
UFW_BOTTOM_EXPECT     equ $00077F9C          ; word
; A Mega STE's speed and cache, as TOS left them (MSTE_CTRL).
UFW_MSTE_CTRL         equ $00077F9E          ; word
; Whether YM samples play (-1); where the samples' cursor A0 starts: the
; audio buffer's ring (it runs on from there, see userfw_vbl), or a zero
; area userfw_vbl points A0 back at each VBL when nothing plays on the YM
; (the copy's writes then land in RAM).
UFW_YM_SAMPLES        equ $00077FA0          ; word
UFW_SAMPLE_BASE       equ $00077FA2          ; longword
; Where the copy's sample writes go (A4): the YM's select register when
; YM samples play, else UFW_SAMPLE_SINK, 12 bytes of RAM a sample's
; movep.l and movep.w land in (no wait states: the writes to the YM cost
; more on the machine, a time a silent ST keeps).
UFW_SAMPLE_PORT       equ $00077FA6          ; longword
UFW_SAMPLE_SINK       equ $00077FBA          ; 12 bytes, up to $77FC5
; The lines copied last frame (COPY_LINES_ADDR): when the RP's word changes,
; the image's bottom lines of both pages are cleared once (a short copy
; never writes them).
UFW_COPY_LINES        equ $00077FB4          ; word
; The samples' cursor at the last VBL (TIME_STUDY: the samples a VBL wrote).
UFW_SAMPLE_LAST       equ $00077FB6          ; longword
UFW_VBL_VEC_SAVE      equ $00077FE0          ; longword: TOS VBL vector ($70)
UFW_PHYSBASE_SAVE     equ $00077FE8          ; longword: XBIOS Physbase result
UFW_SCREEN_PAGE       equ $00077FEC          ; longword: current draw page address
; Low word of FB_FRAME_COUNTER_ADDR at the last blit: userfw_vbl_loop blits only when
; the counter has moved on since (see FB_FRAME_COUNTER_ADDR).
UFW_LAST_FRAME        equ $00077FF0          ; word
; userfw_vbl_loop arms this to -1 then `stop`s; userfw_vbl clears it. The
; m68k re-stops on any non-VBL IRQ (Timer-B etc.) and only exits the
; wait when the VBL handler has cleared the flag. It used to borrow
; TOS's _dskbufp ($4C6); it lives in userfw's own state area now.
UFW_VBL_FLAG          equ $00077FF2          ; word: cleared by userfw_vbl, polled after each `stop`
; The generation of the IKBD commands userfw_vbl_loop is sending (-1 at boot: no
; RP generation has bit 15 set, so the RP's commands are sent after every
; boot), and the next byte to send (-1: all sent and reported).
UFW_IKBD_OUT_GEN      equ $00077FF6          ; word
UFW_IKBD_OUT_STEP     equ $00077FF8          ; word
; IKBD bytes userfw_acia_irq has read; userfw_vbl reports its low byte.
UFW_IKBD_COUNT        equ $00077FFA          ; word

; The copy's shape: per line of the framebuffer, two MOVEMs of 11 longwords
; (D0-D7, A1-A3: A6 is the source, A5 the destination, A0 the audio cursor,
; A4 the YM's address, A7 the SP -- samples are written and interrupts fire
; during the copy) and one of 10 for the rest. The pair count is what the
; cycles depend on, not the registers: 580 a line either way.
FBDRV_ITER_BYTES      equ 44
FBDRV_LINE_CHUNKS     equ 2
FBDRV_LINE_TAIL       equ (FB_LINE_BYTES - FBDRV_LINE_CHUNKS * FBDRV_ITER_BYTES) ; 40: D0-D7/A1-A2

; --- Samples by the copy (FB_INLINE_SAMPLES) ----------------------------
;
; An interrupt per sample costs about 152 cycles, of which the YM writes are
; 48: 112 samples a VBL took 2.1 ms, more than the 256-line copy leaves. So
; at 50 fps the copy writes the samples itself: after every 5th MOVEM pair
; (FBDRV_SAMPLE_EVERY; FBDRV_SAMPLE_LONG_EVERY, when not 0, makes every
; such sample a pair later: 5 pairs, about 1,030 cycles, the nominal 7,777
; Hz), one SAMPLE_WRITE (60
; cycles: the sample's six bytes, as the RP lays them out, to the YM's
; select and data registers with a movep.l and a movep.w through A4 =
; YM_SELECT, the YM's three channels for about 8 bits; the YM mirrors its
; two registers every 4 bytes). Each border handler writes one
; after its switch, since it holds the copy up for about a sample's time
; (written before the switch it ate the margin of the handler's wait), and
; between the copy's end and the next VBL the main loop writes them at the
; same pace (IDLE_SAMPLE_ITERS; a VBL with no new frame plays the whole VBL
; that way), and the gap around the VBL (the VBL handler, the wake, the
; frame check, the copy's first pairs: half a millisecond with none, a 50 Hz
; wobble) gets one at the VBL handler's entry and end, one at the wake, one
; as the copy starts, and the copy's first after FBDRV_FIRST_SAMPLE pairs.
; Per VBL: FBDRV_INLINE_SAMPLES in the copy, 2 by the handlers,
; 3 or 4 idle: about PROFILE_50FPS_YM_SAMPLES_A_VBL, the rate the RP lays
; the ring out for; on the machine the YM's wait states, the handlers and
; the IKBD's interrupts make it about 154 (7,697 Hz against the nominal
; 7,777), and the RP follows: A0 runs on
; through the ring, userfw_vbl reports it, the RP writes ahead of it and
; resamples to the rate the ST achieves (no sample is ever skipped or
; held; the cadence only sets the pitch the RP corrects). On the DMA path
; and when the RP asks for no sound, A0 points at a zero area and A4 at
; RAM (UFW_SAMPLE_SINK): the writes cost 24 cycles each and touch no YM
; (the copy runs there only when the RP forces the CPU, or on a plain ST
; with no sound; the blitter path writes no samples).
FBDRV_SAMPLE_EVERY    equ 5
FBDRV_SAMPLE_LONG_EVERY equ 0                ; none longer
FBDRV_FIRST_SAMPLE    equ 3                  ; pairs before the copy's first sample
FBDRV_INLINE_SAMPLES  equ 153                ; of the 768 pairs: at 3, 8, 13 ... 763
PROFILE_50FPS_YM_SAMPLES_A_VBL equ 155       ; 7,777 Hz x 20 ms
; The idle loop: an iteration of 30 cycles (the VBL flag, dbf), the sample
; and the loop's own 56 or so: a sample every 1,046 cycles. The first delay
; after a copy makes up for the copy's last pairs and the flip (they take
; about a sample's time already).
IDLE_SAMPLE_ITERS     equ 33
IDLE_FIRST_ITERS      equ 0
IDLE_NO_FRAME_ITERS   equ 7

; SAMPLE_WRITE \1: the next sample (A0, six bytes) to the YM (A4 =
; YM_SELECT), through data register \1: channels A and B with a movep.l,
; C with a movep.w at the registers' next mirror. HANDLER_SAMPLE: the same from a border handler, after
; its switch, saving what it uses, when the YM plays.
SAMPLE_WRITE          macro
    move.l  (a0)+, \1
    movep.l \1, 0(a4)
    move.w  (a0)+, \1
    movep.w \1, 8(a4)
                      endm
HANDLER_SAMPLE        macro
    ifne    FB_INLINE_SAMPLES
    tst.w   UFW_YM_SAMPLES
    beq.s   .\@nos
    movem.l d0/a4, -(sp)
    lea     YM_SELECT.w, a4
    SAMPLE_WRITE d0
    movem.l (sp)+, d0/a4
.\@nos:
    endc
                      endm
; FBDRV_PAIR_SAMPLE: after a MOVEM pair of the copy, a sample every
; FBDRV_SAMPLE_EVERY pairs, every FBDRV_SAMPLE_LONG_EVERY-th sample a pair
; later (D0 is free between a store and the next load).
FBDRV_PAIR_SAMPLE     macro
    ifne    FB_INLINE_SAMPLES
FBDRV_TOSAMPLE        set FBDRV_TOSAMPLE-1
    ifeq    FBDRV_TOSAMPLE
    SAMPLE_WRITE d0
FBDRV_TOSAMPLE        set FBDRV_SAMPLE_EVERY
    ifne    FBDRV_SAMPLE_LONG_EVERY
FBDRV_TOLONG          set FBDRV_TOLONG-1
    ifeq    FBDRV_TOLONG
FBDRV_TOSAMPLE        set FBDRV_SAMPLE_EVERY+1
FBDRV_TOLONG          set FBDRV_SAMPLE_LONG_EVERY
    endc
    endc
    endc
    endc
                      endm

;----------------------------------------------------------------
; FBDRV_INLINE -- fully unrolled cart->ST screen framebuffer copy.
;
; From the image's last line in the page to its first, per line:
;   movem.l (a6)+, d0-d7/a1-a3   ; 100 cycles: 44 bytes forward
;   movem.l d0-d7/a1-a3, -(a5)   ;  96 cycles: to the line's bytes 84..127
;   movem.l (a6)+, d0-d7/a1-a3   ; 100
;   movem.l d0-d7/a1-a3, -(a5)   ;  96: bytes 40..83
;   movem.l (a6)+, d0-d7/a1-a2   ;  92
;   movem.l d0-d7/a1-a2, -(a5)   ;  88: bytes 0..39
;   lea     -32(a5), a5          ;   8: the line above, at its byte 128
; 580 cycles a line, 148,480 for 256 lines: 18.5 ms of the 20 ms VBL, and
; at 50 fps a sample every 5 pairs (FBDRV_PAIR_SAMPLE), 9,180 more (the 240
; lines of a plain ST: 139,200 and 8,640).
;
; Predecrement stores are 4 cycles faster than d16(a5) ones (8+8n against
; 12+8n), so the RP lays the framebuffer out in the order this reads it: the
; lines from the last to the first, each as its bytes 84..127, 40..83, 0..39
; ("Framebuffer layout for the m68k's copy" in cart_shared.h).
;
; A7 (SP), A0 and A4 are not in the MOVEM lists: A7 keeps the supervisor SP
; valid so interrupts can fire during the copy, A0 is the audio cursor and
; A4 the YM's address for the samples. An interrupt can fire between a
; MOVEM load and its store, while D0-D7 and A1-A3 hold pixels: every
; handler saves what it uses (A0 aside).
;
; Caller: A5 = the image's last line in the page, at its byte FB_LINE_BYTES;
; A4 = the samples' port (UFW_SAMPLE_PORT). Clobbers D0-D7, A1-A3, A5, A6.
; 28 bytes a line plus the samples, 8 KB. A copy of the top
; FB_COPY_LINES_SHORT lines only (COPY_LINES_ADDR) jumps to fbdrv_short with
; A6 and A5 past the lines it leaves out.
FBDRV_INLINE          macro
    movea.l #FRAMEBUFFER_ADDR, a6
FBDRV_TOSAMPLE        set FBDRV_FIRST_SAMPLE
FBDRV_TOLONG          set FBDRV_SAMPLE_LONG_EVERY
FBDRV_LINE            set 0
    rept    FB_COPY_LINES
    ifeq    FBDRV_LINE-(FB_COPY_LINES-FB_COPY_LINES_SHORT)
fbdrv_short:                              ; the copy of the top FB_COPY_LINES_SHORT lines enters here
    endc
FBDRV_LINE            set FBDRV_LINE+1
    movem.l (a6)+, d0-d7/a1-a3
    movem.l d0-d7/a1-a3, -(a5)
    FBDRV_PAIR_SAMPLE
    movem.l (a6)+, d0-d7/a1-a3
    movem.l d0-d7/a1-a3, -(a5)
    FBDRV_PAIR_SAMPLE
    movem.l (a6)+, d0-d7/a1-a2
    movem.l d0-d7/a1-a2, -(a5)
    FBDRV_PAIR_SAMPLE
    lea     -(FB_ROW_BYTES - FB_LINE_BYTES)(a5), a5
    endr
                      endm

; Atari ST VBL interrupt vector. Replacing TOS's handler here drops
; mouse / cursor-blink / keyboard-repeat updates -- harmless for the
; framebuffer template because we own the screen until ESC exit.
VBL_VECTOR            equ $70

; FB frame counter (FB_FRAME_COUNTER_ADDR, $FA400C). The RP publishes a
; whole frame into the cart framebuffer and then bumps this counter as
; its LAST write. userfw_vbl_loop blits only when the counter's low word has
; changed since the last blit (UFW_LAST_FRAME), and acknowledges only
; after a blit (VBLSYNC_ADDR). The RP starts the next publish only after
; that acknowledgement, so the two never overlap: the ST reads nothing
; between an acknowledgement and the next counter change, and a changed
; counter means the frame is complete. A slow app gets fewer frames,
; never a torn one. The low word is read with one move.w: a longword is
; two bus reads with its halves swapped and could mix old and new.

; RP→m68k command sentinel (CMD_MAGIC_SENTINEL_ADDR, $FA4000). The RP
; IKBD demux writes CMD_BOOT_GEM there when it decodes an ESC keypress;
; userfw's main loop polls and exits back to GEM on match. CMD_RESET comes
; before the RP reboots into Booster: userfw then cold-resets the ST
; (.cold_reset).
; Delay before the cold reset, so TOS scans the cartridge only once Booster
; serves it. Mirrors PRE_RESET_WAIT in main.s, the delay Booster uses too.
UFW_PRE_RESET_WAIT    equ $FFFFF

; The 16-entry ST palette at PALETTE_ADDR, published by the RP:
; userfw_vbl_loop applies it to PALETTE_BASE each frame via a MOVEM-load +
; MOVEM-store.

; --- Overscan ------------------------------------------------------
;
; The ST shows 274 lines: the top border opened (29 lines from line 34, the
; first of a 60 Hz screen) and the bottom one (45 more after line 262, the
; last of a normal screen). Each is opened by being at 60 Hz when the GLUE
; decides, at cycle 504 of line 33 and of line 262, and back at 50 Hz right
; after; the line lengths and the frame stay as at 50 Hz.
;
; The switches have to land within a few cycles, and an interrupt can start
; a hundred cycles late (a MOVEM of the copy runs to its end first). So both
; handlers wait for a line to start with `stop` and the HBL interrupt (whose
; vector is a bare rte), and count cycles from there:
;   - The bottom one, Timer-B counting the display lines, fires near the end
;     of line 259; it waits for line starts until the video counter says it
;     is in line 262, switches, and starts Timer-C, which fires in the next
;     frame's line 31: a frame is 313 lines of 512 cycles, so that delay is
;     exact.
;   - The top one, Timer-C, waits for line 33 to start (the second line start
;     after it fires), switches, and arms Timer-B for the bottom one.
; A frame with no such chain (the first, or after a border was missed)
; arms Timer-B from userfw_vbl, for a screen whose top border stays closed.
; While a handler waits, the ACIA's and the samples' interrupts are masked.
;
; Screen pages: 274 lines of 160 bytes each, the image's 256 lines from
; UFW_IMAGE_LINE (in the middle), the rest cleared once at boot. Below the
; state area, above $60000: the third use of Atari RAM (with the DMA ring).
UFW_PAGE_LINES        equ 274
UFW_PAGE_BYTES        equ (UFW_PAGE_LINES * FB_ROW_BYTES)   ; 43,840
UFW_IMAGE_LINE        equ ((UFW_PAGE_LINES - FB_COPY_LINES) / 2)
UFW_SCREEN_A          equ $00060000
UFW_SCREEN_B          equ $0006B000           ; up to $75B40
UFW_SCREEN_XOR        equ (UFW_SCREEN_A ^ UFW_SCREEN_B)
; Where FBDRV_INLINE starts in a page: the image's last line, at its byte 128.
UFW_COPY_START        equ ((UFW_IMAGE_LINE + FB_COPY_LINES - 1) * FB_ROW_BYTES + FB_LINE_BYTES)

; Timer-B counts display lines up to the end of line 259: from line 34 when
; the top border is open (TIMERB_LINES_OPEN), from line 63 when not. The
; bottom handler then waits for the starts of lines 261 and 262. Counting to
; line 260 left it about 50 cycles on the machine (its entry ended near
; cycle 330 of line 261, past which the wait skips to line 263), and the
; stopwatch's wrap or an IKBD byte made it miss the border once a second:
; a line earlier costs a line of waiting and gives a full line of margin,
; and the sample it wrote at entry (170 cycles with the YM's wait states)
; now comes after the switch: an IKBD byte's interrupt (350 cycles on the
; machine), a MOVEM in flight and the stopwatch's wrap together stay
; within it. At the start of line 262 the video counter is
; BOTTOM_EXPECT bytes from the page.
TBCR_EVENT            equ 8                   ; Timer-B control: event count mode
TIMERB_LINES_OPEN     equ (259 - 34 + 1)
TIMERB_LINES_CLOSED   equ (259 - 63 + 1)
BOTTOM_EXPECT_OPEN    equ ((262 - 34) * FB_ROW_BYTES)
BOTTOM_EXPECT_CLOSED  equ ((262 - 63) * FB_ROW_BYTES)
BOTTOM_LINE_TRIES     equ 4                   ; line starts the bottom handler waits for, at most
; Timer-C, delay mode, from the bottom switch (cycle 8 of line 263) into
; the next frame: 197 x 64 MFP clocks (208.6 CPU cycles each) = 41,096
; cycles, to fire early in line 30 (200 fired at 31/436-480 in Hatari). A
; wait for a line start (`stop`, the HBL) ends as the next line starts when
; it is entered before cycle 330 or so of a line, as the one after when
; later, so the handler cannot know from its entry which line it wakes on,
; and an interrupt in the way (the copy's MOVEMs, an ACIA byte: on the
; machine 100-400 cycles) moved it a line: with 200 it missed line 33 in 2%
; of the frames while the mouse moved. So Timer-C runs on after it fires
; (it reloads its count), and at each wake its count says how many ticks
; ago it fired, 2.45 a line: the handler waits until the count says line 33
; (TOP_WAKE_TICKS_33 or more; TOP_WAKE_TICKS_34 or more is line 34, too
; late). Calibrated in Hatari and on an STE (the stopwatch reports the
; count the switch was made at, STUDY_POINT_TOPWAKE).
TIMERC_TOP_PRESCALER  equ 5                   ; /64
TIMERC_TOP_COUNT      equ 197
TOP_WAKE_TICKS_33     equ 5
TOP_WAKE_TICKS_34     equ 7
TOP_LINE_TRIES        equ 4                   ; line starts the top handler waits for, at most
; The GLUE opens a border when the frequency turns to 60 Hz on its line
; (line 33 for the top, 262, the last display line, for the bottom) at
; cycle 500 or before (502 on an STF), and back to 50 Hz after. NOPs from
; each handler's wake to its 60 Hz write (at cycle 464 of the line), and
; from there to the 50 Hz one (at cycle 8 of the next line): tuned in
; Hatari (--trace video_sync, tools/re/scripts/overscan.exp) and checked on
; the machines. The bottom handler reads the counter after its wake.
TOP_DELAY_NOPS        equ 71
BOTTOM_DELAY_NOPS     equ 69
BORDER_HOLD_NOPS      equ 10

; A Mega STE runs at 8 MHz with its cache off while userfw does: the
; switches are counted in 8 MHz cycles.
MSTE_CTRL             equ $FFFF8E21           ; bit 0 cache, bit 1 16 MHz
MACHINE_MEGA_STE      equ $11

; --- YM2149 sound chip (channels A and B as a fake DAC) --------------
;
; PSG access: write a register number to $FFFF8800 (latch), then
; write data to $FFFF8802. Regs 8 and 9 = ch A and ch B volume (low 4
; bits). Both channels are a "fake DAC": tone enabled, period = 0 (DC
; clamp above the audio band so the volume registers are the only
; thing driving the output). Reg 8 is latched at boot; each Timer-B
; fire writes ch A, latches reg 9, writes ch B and latches reg 8 again.
YM_SELECT             equ $FFFF8800
YM_DATA               equ $FFFF8802
YM_REG_MIXER          equ 7                  ; tone+noise enables
YM_REG_CHA_VOL        equ 8                  ; channel A volume (low 4 bits)
YM_REG_CHB_VOL        equ 9                  ; channel B volume (low 4 bits)
YM_REG_CHC_VOL        equ 10                 ; channel C volume (low 4 bits)
YM_MIXER_DAC_CHA      equ $FE                ; tone A enabled, all other tones/noise off, ports out
YM_MIXER_DAC_AB       equ $FC                ; tones A AND B enabled, tone C off, all noise off, ports out (Ghostbusters dual-channel fake DAC)
YM_MIXER_DAC_STEADY   equ $FF                ; all tones and noise off, ports out: each channel holds its volume's level (three channels' DAC, as measured)

; The audio buffer at AUDIO_BUFFER_ADDR: on the YM path a ring of samples
; the cursor A0 runs through (six bytes a sample at 50 fps, the three
; channels' volumes, the (vA, vB) pair at 25), on into the spill after it; userfw_vbl brings it back and
; reports it. Filled by the RP, ahead of the cursor.

; FB_COPY_LINES (inc/sidecart_layout.s) is the framebuffer's lines, 256 of
; 256 pixels: the first FB_LINE_BYTES of each of the page's lines from
; UFW_IMAGE_LINE. The RP can ask for the top FB_COPY_LINES_SHORT only
; (COPY_LINES_ADDR): the bottom 16 lines of the image then stay black, and
; the copy is 16 x 580 cycles shorter.

; --- IKBD ownership ------------------------------------------------

; Keyboard ACIA at $FFFFFC00/02. MIDI ACIA at $FFFFFC04/06 is not
; touched. Status bit 0 = RX-data-ready; bit 1 = TX-empty.
ACIA_KBD_STATUS       equ $FFFFFC00
ACIA_KBD_DATA         equ $FFFFFC02

; IKBD commands. What the IKBD reports (keys only, the mouse, the
; joysticks) is the RP's choice: userfw_vbl_loop sends the commands the RP puts at
; IKBD_OUT_ADDR, and the RP decodes whatever the IKBD sends.
;
; For a while after a reset the IKBD undoes a mouse-off command, and TOS
; resets it moments before this cartridge code runs: on a cold boot a $12
; sent then was sometimes lost and the mouse stayed on (seen on a Mega ST).
; So userfw resets the IKBD itself, waits for the reset's answer ($F0, $F1
; on a later IKBD release), throws it away and lets IKBD_SETTLE_ITERS pass
; before userfw_vbl_loop sends the RP's commands. On the way out, TOS gets back
; what it set up: joystick events, then the relative mouse. The order
; matters: $14 also turns the mouse off.
IKBD_CMD_RESET_HDR    equ $80    ; reset command header (followed by $01)
IKBD_CMD_RESET_RUN    equ $01    ; reset + self-test; answers $F0 (or $F1)
IKBD_CMD_MOUSE_REL    equ $08    ; relative mouse reporting (TOS's mode)
IKBD_CMD_JOY_EVENTS   equ $14    ; joystick event reporting (TOS's mode)

; Waiting for the reset's answer: one pass of the wait loop is about 44
; cycles, so 400,000 passes are 0.55 s at 32 MHz and 2.2 s at 8 MHz. The
; IKBD answers after its self-test (about 60 ms); the timeout only matters
; when nothing answers (no keyboard). The settle delay after the answer
; (2 x 18 cycles per pass) is 20 ms at 32 MHz, 80 ms at 8 MHz.
IKBD_RESET_TIMEOUT    equ 400000
IKBD_SETTLE_ITERS     equ 36000

; Send one command byte to the IKBD once the ACIA can take it (status bit 1,
; TX-empty). \1: the byte.
IKBD_SEND             macro
.\@wait:
    btst    #1, ACIA_KBD_STATUS.w
    beq.s   .\@wait
    move.b  #\1, ACIA_KBD_DATA.w
                      endm

; MC68901 MFP registers (subset we manipulate).
MFP_IERA              equ $FFFFFA07          ; interrupt enable A (Timer-A = bit 5)
MFP_IERB              equ $FFFFFA09          ; interrupt enable B
MFP_IPRA              equ $FFFFFA0B          ; interrupt pending A (Timer-B = bit 0)
MFP_ISRA              equ $FFFFFA0F          ; in-service A (Timer-A ack = bit 5)
MFP_IMRA              equ $FFFFFA13          ; interrupt mask A
MFP_IMRB              equ $FFFFFA15          ; interrupt mask B
MFP_VR                equ $FFFFFA17          ; vector register (high nibble = vector base, bit 3 = S: 1=software EOI, 0=auto-EOI)
MFP_TACR              equ $FFFFFA19          ; Timer-A control register (cleared at boot for safety)
MFP_TBCR              equ $FFFFFA1B          ; Timer-B control register (delay-mode + prescaler)
MFP_TBDR              equ $FFFFFA21          ; Timer-B data register (8-bit countdown)
MFP_TCDCR             equ $FFFFFA1D          ; Timer-C (bits 6-4) and Timer-D (bits 2-0) control
MFP_TCDR              equ $FFFFFA23          ; Timer-C data register
MFP_TDDR              equ $FFFFFA25          ; Timer-D data register
MFP_TIMERB_BIT        equ 0                  ; IERA / IMRA bit of Timer-B
MFP_TIMERD_BIT        equ 4                  ; IERB / IMRB bit of Timer-D
MFP_TIMERC_BIT        equ 5                  ; IERB / IMRB bit of Timer-C
MFP_TIMERA_BIT        equ 5                  ; IERA / IMRA bit of Timer-A

; The YM samples' rate, and everything that follows from it. Timer-D plays
; them (Timer-B and Timer-C open the borders); the profile's count keeps its
; name, PROFILE_*_TIMERB_COUNT. MFP master clock = 2.4576 MHz, /4 prescaler,
; the profile's count (PROFILE_ADDR):
;   PROFILE_50FPS:  79 -> 7,777.2 Hz, 155.4 samples per PAL VBL (at 50 fps
;      the copy writes them at that cadence, not a timer);
;   PROFILE_25FPS:  28 -> 21,942.86 Hz, 438.2 samples per PAL VBL.
;   -> in the ring (AUDIO_YM_RING_BYTES, inc/sidecart_layout.s): 6 bytes a
;      sample at 50 fps, 2 (vA, vB) at 25 (the RP's PROFILE_YM_SAMPLE_BYTES,
;      rp/src/include/profile.h);
;   -> the RP's rates (cart_shared.h CART_PROFILE_*_YM_RATE_HZ), which the
;      ST's reports correct to what it achieves. YM sources (.YMS files, the
;      built-in jingle) stay at 5,585 Hz: the RP resamples.
; tests/host/test_layout.py checks that those places agree.
YM_TIMER_PRESCALER    equ 1                  ; /4 (delay mode)

; STE / Mega STE DMA sound. The chip plays 8-bit samples from ST RAM only,
; so userfw keeps a ring of AUDIO_DMA_RING_BYTES there, the second use of
; Atari RAM after the screen pages and on the same assumption: the 4 KB
; between screen page B and the state area. The chip loops over it mono, at the profile's rate
; (25,033 Hz in both profiles); every VBL userfw copies into it, from the audio
; buffer that mirrors it, what the RP has written ahead of the chip (see
; AUDIO_BUFFER_ADDR).
DMA_SND_CTRL          equ $FFFF8901          ; bit 0 play, bit 1 loop
DMA_SND_START         equ $FFFF8903          ; frame start: high, mid (+2), low (+4)
DMA_SND_COUNT_MID     equ $FFFF890B          ; where it plays (read only): middle byte
DMA_SND_COUNT_LOW     equ $FFFF890D          ;   and low byte
DMA_SND_END           equ $FFFF890F          ; frame end: high, mid (+2), low (+4)
DMA_SND_MODE          equ $FFFF8921          ; bits 1-0 rate, bit 7 mono
DMA_CTRL_LOOP_PLAY    equ 3
UFW_DMA_RING          equ $00076000          ; AUDIO_DMA_RING_BYTES, up to $76FFF
MACHINE_FAMILY_STE    equ $1                 ; the hello byte's family: STE, Mega STE
; The LMC1992 behind the Microwire: the DMA sound goes through it. userfw
; sets what TOS sets at boot, in case something changed it: master, left
; and right at 0 dB, bass and treble flat, the YM mixed in (GEM's bell).
MW_DATA               equ $FFFF8922
MW_MASK               equ $FFFF8924
MW_MASK_ALL           equ $07FF              ; also the mask at rest: a transfer ended
MW_WAIT_ITERS         equ 2000               ; a transfer takes 16 us

; The blitter (an STE, a Mega STE, a Mega ST fitted with one): its registers
; from BLT_BASE ($FFFF8A20; the halftone RAM below it is not used). The copy
; is HOP 2 (source) and OP 3 (source), a plain word copy, in hog mode: the
; blitter owns the bus, the CPU and its interrupts wait. It needs 8 cycles a
; word against MOVEM's 8.8 and nothing per line: a line of 64 words takes
; 512 cycles, one scanline, so the whole frame takes 256 of the 313. It reads
; the framebuffer's lines in natural order (the RP publishes that layout for
; it, cart_shared.h) and writes each to its screen line, the destination
; skipping the bytes right of the image (BLT_DST_Y_INC). A piece of lines at
; a time, so that the border switches and the IKBD get the CPU: the main loop
; sizes each piece from the timers (.copy_blitter).
BLT_BASE              equ $FFFF8A20
BLT_SRC_X_INC         equ $FFFF8A20
BLT_SRC_Y_INC         equ $FFFF8A22
BLT_SRC_ADDR          equ $FFFF8A24
BLT_ENDMASK1          equ $FFFF8A28
BLT_ENDMASK2          equ $FFFF8A2A
BLT_ENDMASK3          equ $FFFF8A2C
BLT_DST_X_INC         equ $FFFF8A2E
BLT_DST_Y_INC_REG     equ $FFFF8A30
BLT_DST_ADDR          equ $FFFF8A32
BLT_X_COUNT           equ $FFFF8A36
BLT_Y_COUNT           equ $FFFF8A38
BLT_HOP               equ $FFFF8A3A          ; word: HOP (high byte), OP (low byte)
BLT_CTRL              equ $FFFF8A3C          ; bit 7 busy (start), bit 6 hog
BLT_SKEW              equ $FFFF8A3D
BLT_HOP_OP_COPY       equ $0203
BLT_BUSY_HOG          equ $C0          ; control: start, and own the bus
BLT_DST_Y_INC         equ (FB_ROW_BYTES - FB_LINE_BYTES + 2)
; Lines the main loop leaves before a border handler fires: Timer-C's count
; gives them as an estimate (TIMERC_LINES_A_TICK_256 / 256 lines a tick, at
; /64, rounded down), Timer-B's counts display lines exactly. A margin of a
; line: a piece then ends at least half a line before the fire, or, when the
; count moves between its read and the piece's start, at the fire's own
; line, which the handlers tolerate (they are entered within a line). Each
; piece's start costs about 165 cycles (the loop, the registers, the bus).
TIMERC_LINES_A_TICK_256 equ 104              ; 208.6 cycles a tick / 512 a line
BLIT_MARGIN_TIMERC    equ 1
BLIT_MARGIN_TIMERB    equ 1

; IRQ vector slots we take over. $70 (VBL) already handled by the
; original userfw code path (D3 holds the save).
VEC_HBL               equ $68
VEC_TIMERD            equ $110
VEC_TIMERC            equ $114
VEC_ACIA              equ $118
VEC_TIMERB            equ $120
VEC_TIMERA            equ $134

; IKBD cart-bus emit window (ROM3). The ACIA interrupt
; (userfw_acia_irq) reads (IKBD_WINDOW_BASE + byte).b to forward `byte`
; to RP; the RP side filters commemul ring samples whose low 16 bits
; fall in [$8200, $8300) and extracts the IKBD byte from the low 8
; bits.

; The keyboard ACIA's receive interrupt reads every IKBD byte as it
; arrives (userfw_acia_irq). The 6850 holds one byte and the IKBD sends
; one every 1.28 ms: polling from the blit left gaps longer than that
; (between blits, and when audio interrupts stretched the poll interval),
; and a second byte overran the first -- md-oric measured overruns and a
; stuck key the same way. When the ACIA reports an overrun (status bit 5)
; the handler also reads IKBD_OVERRUN_ADDR, which the RP counts.
; The MIDI ACIA shares the GPIP4 line: its receive interrupt is turned
; off while userfw runs, or a MIDI byte nobody reads would hold the line
; down and stop the keyboard.
ACIA_MIDI_CTRL        equ $FFFFFC04
ACIA_KBD_CTRL_TOS     equ $96        ; RX interrupt on, 8N1, /64 (TOS's setting)
ACIA_MIDI_CTRL_OFF    equ $15        ; RX interrupt off, 8N1, /16
ACIA_MIDI_CTRL_TOS    equ $95        ; RX interrupt on, 8N1, /16 (TOS's setting)
MFP_GPIP4_BIT         equ 6          ; IERB / IMRB bit of the ACIA interrupt

; VBL frame-sync ack. After each blit completes (userfw_after_copy)
; the m68k does a single dummy cart-bus read at VBLSYNC_ADDR to tell
; the RP "the blit is done, the cart framebuffer is free to overwrite".
; The m68k cannot WRITE the shared region (it's ROM from the m68k
; side), so the ack must be a READ captured by the RP's commemul ring
; -- the same mechanism IKBD uses. Distinct high byte ($84) from the
; IKBD window ($82) so the RP can tell the two apart. The value read
; is irrelevant; only the address matters.

; Hello (rp/src/include/st_session.h): the ST and the RP reboot
; independently and the RP keeps its state across an ST reset, the command
; sentinel included, so after an exit to GEM the next boot would read
; CMD_BOOT_GEM and leave at once. userfw says hello at every boot with three
; ROM3 reads, the TOS version's two bytes then the machine; on the hello the
; RP writes CMD_NOP to the sentinel and starts its session over. Windows and
; the machine byte: see cart_shared.h and st_session.h.
P_COOKIES             equ $5A0        ; _p_cookies: the cookie jar, 0 on TOS 1.0x
RESET_VECTOR_HI       equ $4          ; high word of the reset PC: $00FC on a 192 KB TOS
TOS_ROM_192K          equ $FC0000     ; TOS header (version word at +2), 192 KB TOS
TOS_ROM_256K          equ $E00000     ; TOS header, 256 KB TOS and later

; Save area for vectors + MFP regs we'll restore on ESC exit, at
; UFW_SAVE_AREA: between the DMA sound ring and the state area, in the
; RAM GAME.TOS keeps for userfw (main.s: GAME_RAM_LOW..GAME_RAM_HIGH).
; A5 holds the pointer while the vectors are saved; the exit path loads it
; again in case anything clobbered A5.
;   offset  0: $68  HBL vector save (long)
;   offset  4: $110 Timer-D vector save (long)
;   offset  8: $114 Timer-C vector save (long)
;   offset 12: $118 ACIA vector save (long)
;   offset 16: $120 Timer-B vector save (long)
;   offset 20: $134 Timer-A vector save (long)
;   offset 24: MFP IERA save (byte)
;   offset 25: MFP IERB save (byte)
;   offset 26: MFP IMRA save (byte)
;   offset 27: MFP IMRB save (byte)
;   offset 28: MFP VR save (byte) -- S-bit + vector base, switched to auto-EOI under userfw
;   offset 29: MFP TCDCR save (byte) -- TOS's 200 Hz Timer-C, and Timer-D
;   offset 30: MFP TCDR save (byte)
;   offset 31: MFP TDDR save (byte)
UFW_SAVE_SIZE         equ 32
UFW_SAVE_AREA         equ $00077E00          ; up to $77E1F

    section text

userfw:
    ; --- Boot setup (runs once) ---

    ; Hello (see ST_HELLO_WINDOW). The TOS version comes from the ROM
    ; header, as md-microfirmware-template reads it; the machine from the
    ; _MCH cookie. The RP clears the sentinel on the hello, well before
    ; userfw_vbl_loop first reads it: the IKBD reset below waits for the
    ; keyboard's answer first.
    lea     TOS_ROM_192K+2, a0
    cmpi.w  #(TOS_ROM_192K >> 16), RESET_VECTOR_HI.w
    beq.s   .hello_tos
    lea     TOS_ROM_256K+2, a0
.hello_tos:
    move.w  (a0), d1                      ; TOS version, e.g. $0206
    moveq   #0, d0
    move.b  d1, d0                        ; low byte
    lsr.w   #8, d1                        ; high byte
    lea     ST_TOS_HI_WINDOW, a0
    tst.b   (a0, d1.w)
    lea     ST_TOS_LO_WINDOW, a0
    tst.b   (a0, d0.w)
    moveq   #0, d0                        ; no cookie jar: an ST
    move.l  P_COOKIES.w, d1
    beq.s   .hello_send
    movea.l d1, a0
.hello_cookie:
    move.l  (a0)+, d1
    beq.s   .hello_send                   ; end of the jar, no _MCH: an ST
    cmpi.l  #'_MCH', d1
    beq.s   .hello_mch
    addq.w  #4, a0
    bra.s   .hello_cookie
.hello_mch:
    move.l  (a0), d1                      ; $000F00mm: F family, mm $10 on a Mega STE
    move.l  d1, d0
    swap    d0
    lsl.w   #4, d0                        ; family in bits 7..4
    lsr.w   #4, d1
    andi.w  #$000F, d1                    ; model in bits 3..0
    or.w    d1, d0
    andi.w  #$00FF, d0
.hello_send:
    lea     ST_HELLO_WINDOW, a0
    tst.b   (a0, d0.w)
    move.w  d0, UFW_MACHINE

    ; Save the original screen base so we can restore it on ESC exit.
    move.w  #2, -(sp)                ; XBIOS Physbase
    trap    #14
    addq.l  #2, sp
    move.l  d0, UFW_PHYSBASE_SAVE    ; saved screen base lives in RAM now

    ; A blitter? XBIOS Blitmode(-1): bit 1 set when there is one (an STE,
    ; a Mega STE, a Mega ST fitted with one). Told to the RP once.
    move.w  #-1, -(sp)
    move.w  #64, -(sp)                ; XBIOS Blitmode
    trap    #14
    addq.l  #4, sp
    moveq   #0, d1
    btst    #1, d0
    beq.s   .no_blitter
    moveq   #ST_FEATURE_BLITTER, d1
.no_blitter:
    move.w  d1, UFW_HAS_BLITTER
    lea     ST_FEATURES_WINDOW, a0
    tst.b   (a0, d1.w)

    ; Save TOS's VBL vector and install ours. We're in supervisor mode
    ; (GAME.TOS entered it, main.s) so writing $70.w is legal.
    move.l  VBL_VECTOR.w, UFW_VBL_VEC_SAVE   ; TOS VBL vector saved in RAM
    lea     userfw_vbl(pc), a0
    move.l  a0, VBL_VECTOR.w

    ; --- IKBD ownership setup ------------------------------------
    ;
    ; A5 = save area pointer (UFW_SAVE_AREA). Used at boot to save
    ; the 6 IRQ vectors + MFP IER/IMR; ESC exit loads A5 again before
    ; reading the save area, so A5 doesn't need to survive the per-VBL
    ; FBDRV_INLINE expansion.
    lea     UFW_SAVE_AREA, a5

    ; The command sentinel at CMD_MAGIC_SENTINEL_ADDR is RP-owned (m68k
    ; can't write to the cart shared region) and is zeroed by the
    ; RP's ERASE_FIRMWARE_IN_RAM at boot, so we don't need to clear
    ; it from here. It's already CMD_NOP=0 on first userfw entry.

    ; Mask all maskable IRQs while we rewrite vectors + MFP state.
    move.w  sr, -(sp)
    ori.w   #$0700, sr

    ; Save the 6 vectors we're about to overwrite ($70 already saved
    ; to UFW_VBL_VEC_SAVE above).
    move.l  VEC_HBL.w, 0(a5)
    move.l  VEC_TIMERD.w, 4(a5)
    move.l  VEC_TIMERC.w, 8(a5)
    move.l  VEC_ACIA.w, 12(a5)
    move.l  VEC_TIMERB.w, 16(a5)
    move.l  VEC_TIMERA.w, 20(a5)

    ; Save MFP IER / IMR for A and B (4 bytes), plus VR (1 byte).
    move.b  MFP_IERA.w, 24(a5)
    move.b  MFP_IERB.w, 25(a5)
    move.b  MFP_IMRA.w, 26(a5)
    move.b  MFP_IMRB.w, 27(a5)
    move.b  MFP_VR.w, 28(a5)             ; TOS uses S=1 (software EOI); we override below
    move.b  MFP_TCDCR.w, 29(a5)
    move.b  MFP_TCDR.w, 30(a5)
    move.b  MFP_TDDR.w, 31(a5)

    ; Install dummies at HBL / Timer-A / Timer-B / Timer-C / Timer-D
    ; / ACIA. userfw_dummy_irq is a single rte; PC-relative for the
    ; same runtime-vs-link-address reason userfw_vbl uses lea(pc).
    ; With IERA/IERB cleared below no MFP source actually fires, but
    ; the dummies cover the boot window between vector install and
    ; the IERA/IERB clears.
    lea     userfw_dummy_irq(pc), a0
    move.l  a0, VEC_HBL.w
    move.l  a0, VEC_TIMERD.w
    move.l  a0, VEC_TIMERC.w
    move.l  a0, VEC_ACIA.w
    move.l  a0, VEC_TIMERB.w
    move.l  a0, VEC_TIMERA.w

    ; Stop the timers (kills any prior TOS event).
    clr.b   MFP_TBCR.w
    clr.b   MFP_TACR.w
    clr.b   MFP_TCDCR.w

    ; Disable + mask everything in MFP A/B. We re-enable Timer-B
    ; explicitly below; everything else stays off.
    clr.b   MFP_IERA.w
    clr.b   MFP_IERB.w
    clr.b   MFP_IMRA.w
    clr.b   MFP_IMRB.w

    ; Reset the IKBD, wait for its answer and let it settle (see
    ; IKBD_CMD_RESET_HDR); userfw_vbl_loop then sends it the RP's commands.
    ; Interrupts are masked, so nothing else reads the ACIA meanwhile;
    ; every byte that arrives before the answer (a mouse packet in flight,
    ; a key) is read and dropped.
    IKBD_SEND IKBD_CMD_RESET_HDR
    IKBD_SEND IKBD_CMD_RESET_RUN
    move.l  #IKBD_RESET_TIMEOUT, d0
.ikbd_reset_wait:
    btst    #0, ACIA_KBD_STATUS.w         ; a byte waiting?
    beq.s   .ikbd_reset_next
    move.b  ACIA_KBD_DATA.w, d1           ; read it (clears RX-ready)
    andi.b  #$FE, d1
    cmpi.b  #$F0, d1                      ; the answer: $F0 or $F1
    beq.s   .ikbd_reset_answered
.ikbd_reset_next:
    subq.l  #1, d0
    bne.s   .ikbd_reset_wait
.ikbd_reset_answered:
    move.l  #IKBD_SETTLE_ITERS, d0
.ikbd_settle:
    subq.l  #1, d0
    bne.s   .ikbd_settle

    ; Nothing sent to the IKBD yet: userfw_vbl_loop sends the RP's commands and
    ; reports them (see UFW_IKBD_OUT_GEN). No IKBD byte read yet.
    move.w  #-1, UFW_IKBD_OUT_GEN
    move.w  #-1, UFW_IKBD_OUT_STEP
    clr.w   UFW_IKBD_COUNT

    ; Keyboard bytes by interrupt from here on (see IKBD_OVERRUN_ADDR).
    ; SR is still IPL 7, so nothing fires until the loop starts.
    lea     userfw_acia_irq(pc), a1
    move.l  a1, VEC_ACIA.w
    move.b  #ACIA_KBD_CTRL_TOS, ACIA_KBD_STATUS.w
    move.b  #ACIA_MIDI_CTRL_OFF, ACIA_MIDI_CTRL.w
    bset    #MFP_GPIP4_BIT, MFP_IERB.w
    bset    #MFP_GPIP4_BIT, MFP_IMRB.w

    ; --- YM2149 init: ch A + ch B as Ghostbusters dual-channel DAC
    ; Enable tones on BOTH ch A and ch B (mixer bits 0,1 = 0). Tone
    ; periods all 0 so the counters run at max -- effectively DC
    ; clamp above the audio band, so the volume registers alone
    ; shape each channel's output. The two channels sum
    ; acoustically; the Timer-B handler writes a (vA, vB) pair per
    ; fire, and the Ghostbusters 64-entry hand-tuned LUT picks the
    ; pair that best approximates the desired linear amplitude on
    ; the YM's logarithmic volume curve.
    ;
    ; Reg 8 (ch A volume) is latched LAST so the first Timer-B fire
    ; can write ch A immediately; the handler toggles to reg 9
    ; (ch B) mid-fire and back to reg 8 at the end.
    move.b  #YM_REG_MIXER, YM_SELECT.w
    ifne    FB_INLINE_SAMPLES
    ; Three channels: tones off, so each holds the level the RP's table
    ; was fitted to (measured that way, rp/src/audio.c audioYmTriples).
    move.b  #YM_MIXER_DAC_STEADY, YM_DATA.w ; $FF: all tones and noise off, ports out
    else
    move.b  #YM_MIXER_DAC_AB, YM_DATA.w   ; $FC: tones A+B on, tone C off, all noise off, ports out
    endc
    move.b  #0, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R0 = ch A fine period
    move.b  #1, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R1 = ch A coarse period
    move.b  #2, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R2 = ch B fine period
    move.b  #3, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R3 = ch B coarse period
    move.b  #4, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R4 = ch C fine period
    move.b  #5, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R5 = ch C coarse period
    move.b  #YM_REG_CHC_VOL, YM_SELECT.w  ; latch reg 10 to zero ch C
    move.b  #0, YM_DATA.w                 ; ch C vol = 0 (silence)
    move.b  #YM_REG_CHB_VOL, YM_SELECT.w  ; latch reg 9 to zero ch B
    move.b  #0, YM_DATA.w                 ; ch B vol = 0 (silence)
    move.b  #YM_REG_CHA_VOL, YM_SELECT.w  ; latch reg 8 (next YM_DATA writes hit ch A volume)
    move.b  #0, YM_DATA.w                 ; ch A vol = 0 (silence)

    ; --- The borders' timers, and the samples' -------------------
    ; Timer-B ($120) and Timer-C ($114) open the borders (see "Overscan"),
    ; enabled here and started each frame; on the YM path (.sound_ym below)
    ; Timer-D ($110) plays the samples. SR is still IPL=7 at this point
    ; (set by `ori.w #$0700, sr` above), so no IRQ fires until SR is
    ; dropped to $2300 below.
    ;
    ; Also flip the MFP Vector Register to AUTO-EOI mode (clear
    ; the S bit, VR bit 3). With S=0 the MFP clears its own in-
    ; service bit on each IACK cycle, so the samples' handler can
    ; skip the explicit in-service ACK -- saves
    ; ~12 cyc per IRQ * ~112 IRQ/VBL = ~1300 cyc/VBL.
    lea     userfw_bottom_border(pc), a0
    move.l  a0, VEC_TIMERB.w
    lea     userfw_top_border(pc), a0
    move.l  a0, VEC_TIMERC.w
    bset    #MFP_TIMERB_BIT, MFP_IERA.w   ; the borders' timers (see "Overscan"),
    bset    #MFP_TIMERB_BIT, MFP_IMRA.w   ; started each frame
    bset    #MFP_TIMERC_BIT, MFP_IERB.w
    bset    #MFP_TIMERC_BIT, MFP_IMRB.w
    move.b  28(a5), d0                    ; copy TOS's VR (saved above)
    andi.b  #$F7, d0                      ; clear bit 3 (S) -> auto-EOI
    move.b  d0, MFP_VR.w

    ; A0, the samples' cursor, starts at the ring's start. A0 is in no
    ; MOVEM list and no other code in userfw touches it after this point
    ; (userfw_vbl wraps it with the interrupts masked), so whoever writes
    ; the samples (the copy, the handlers, the idle loop, or Timer-D at
    ; 25 fps) reads through it with no save and restore.
    movea.l #AUDIO_BUFFER_ADDR, a0
    ifne    TIME_STUDY
    move.l  a0, UFW_SAMPLE_LAST
    endc

    ; The app's profile (PROFILE_ADDR, written by the RP before we booted):
    ; the frames a VBL, Timer-B's count, the DMA chip's mode and lead.
    lea     .profile_50(pc), a1
    cmpi.w  #PROFILE_25FPS, PROFILE_ADDR
    bne.s   .profile_read
    lea     .profile_25(pc), a1
.profile_read:
    move.w  (a1)+, UFW_VBLS_A_FRAME
    move.w  (a1)+, UFW_TIMERB_COUNT
    move.w  (a1)+, UFW_DMA_MODE
    move.w  (a1)+, UFW_DMA_LEAD
    clr.w   UFW_VBL_COUNT
    clr.w   UFW_FRAME_VBL
    bra.s   .profile_done
.profile_50:
    dc.w    PROFILE_50FPS_VBLS, PROFILE_50FPS_TIMERB_COUNT
    dc.w    PROFILE_50FPS_DMA_MODE, PROFILE_50FPS_DMA_LEAD
.profile_25:
    dc.w    PROFILE_25FPS_VBLS, PROFILE_25FPS_TIMERB_COUNT
    dc.w    PROFILE_25FPS_DMA_MODE, PROFILE_25FPS_DMA_LEAD
.profile_done:

    ; The sound goes out through the DMA chip on an STE or a Mega STE,
    ; unless the RP keeps us to the YM (AUDIO_OUT_ADDR); through Timer-B and
    ; the YM elsewhere. The RP applies the same rule to what it writes.
    clr.w   UFW_AUDIO_DMA
    move.w  #-1, UFW_COPY_LINES           ; the first frame clears the pages' bottoms
    clr.w   UFW_YM_SAMPLES                ; until .sound_ym: nothing on the YM,
    move.l  #UFW_SCREEN_A, UFW_SAMPLE_BASE ; the copy's writes read zeros
    move.l  #UFW_SAMPLE_SINK, UFW_SAMPLE_PORT ; and go to RAM
    cmpi.w  #AUDIO_OUT_NONE, AUDIO_OUT_ADDR
    beq     .sound_done                   ; no sound at all
    cmpi.w  #AUDIO_OUT_YM, AUDIO_OUT_ADDR
    beq     .sound_ym
    move.w  UFW_MACHINE, d0
    lsr.w   #4, d0
    cmpi.w  #MACHINE_FAMILY_STE, d0
    bne     .sound_ym

    ; DMA: stopped while it is set up; the ring silent (signed 0); the
    ; frame is the whole ring, looped; the mixer as TOS sets it; play.
    clr.b   DMA_SND_CTRL.w
    lea     UFW_DMA_RING, a1
    move.w  #(AUDIO_DMA_RING_BYTES / 4) - 1, d0
.dma_clear:
    clr.l   (a1)+
    dbf     d0, .dma_clear
    move.l  #UFW_DMA_RING, d0
    move.b  d0, DMA_SND_START+4.w
    lsr.l   #8, d0
    move.b  d0, DMA_SND_START+2.w
    lsr.l   #8, d0
    move.b  d0, DMA_SND_START.w
    move.l  #(UFW_DMA_RING + AUDIO_DMA_RING_BYTES), d0
    move.b  d0, DMA_SND_END+4.w
    lsr.l   #8, d0
    move.b  d0, DMA_SND_END+2.w
    lsr.l   #8, d0
    move.b  d0, DMA_SND_END.w
    move.b  UFW_DMA_MODE+1, DMA_SND_MODE.w
    lea     userfw_mw_cmds(pc), a1
    moveq   #((userfw_mw_cmds_end - userfw_mw_cmds) / 2) - 1, d1
.mw_next:
    move.w  #MW_WAIT_ITERS, d2
.mw_wait:
    cmpi.w  #MW_MASK_ALL, MW_MASK.w       ; the last transfer is over
    beq.s   .mw_send
    dbf     d2, .mw_wait
.mw_send:
    move.w  #MW_MASK_ALL, MW_MASK.w
    move.w  (a1)+, MW_DATA.w
    dbf     d1, .mw_next
    clr.w   UFW_DMA_FRONT
    move.b  #DMA_CTRL_LOOP_PLAY, DMA_SND_CTRL.w
    move.w  #-1, UFW_AUDIO_DMA
    bra.s   .sound_done

.sound_ym:
    move.w  #-1, UFW_YM_SAMPLES
    move.l  #AUDIO_BUFFER_ADDR, UFW_SAMPLE_BASE
    move.l  #YM_SELECT, UFW_SAMPLE_PORT
    ifeq    FB_INLINE_SAMPLES
    lea     userfw_timerd_audio(pc), a1   ; the samples on Timer-D (25 fps)
    move.l  a1, VEC_TIMERD.w
    move.b  UFW_TIMERB_COUNT+1, MFP_TDDR.w
    move.b  #YM_TIMER_PRESCALER, MFP_TCDCR.w
    bset    #MFP_TIMERD_BIT, MFP_IERB.w
    bset    #MFP_TIMERD_BIT, MFP_IMRB.w
    endc
.sound_done:
    ifne    TIME_STUDY
    lea     userfw_timera_tick(pc), a1    ; the stopwatch (see TIME_STUDY)
    move.l  a1, VEC_TIMERA.w
    clr.b   MFP_TACR.w
    clr.b   MFP_TADR.w                    ; 256 ticks a wrap
    clr.w   UFW_SW_WRAPS
    bset    #5, MFP_IERA.w                ; its wraps (Timer-A)
    bset    #5, MFP_IMRA.w
    move.b  #TIMERA_STUDY_DIV10, MFP_TACR.w
    endc

    ; A Mega STE at 8 MHz, its cache off (see MSTE_CTRL); restored on exit.
    cmpi.w  #MACHINE_MEGA_STE, UFW_MACHINE
    bne.s   .mste_done
    move.b  MSTE_CTRL.w, UFW_MSTE_CTRL+1
    clr.b   MSTE_CTRL.w
.mste_done:

    ; Both screen pages cleared, the borders' lines with them; no chain of
    ; borders yet (userfw_vbl starts one).
    lea     UFW_SCREEN_A, a1
    move.w  #(UFW_PAGE_BYTES / 4) - 1, d0
.clear_a:
    clr.l   (a1)+
    dbf     d0, .clear_a
    lea     UFW_SCREEN_B, a1
    move.w  #(UFW_PAGE_BYTES / 4) - 1, d0
.clear_b:
    clr.l   (a1)+
    dbf     d0, .clear_b
    clr.w   UFW_BORDERS
    clr.w   UFW_TOP_ARMED

    ; Interrupts back on (caller's level, typically $2300).
    move.w  (sp)+, sr

    ; Initialise the hidden-page pointer. UFW_SCREEN_PAGE holds the
    ; page currently being drawn into; userfw_after_copy toggles it between
    ; UFW_SCREEN_A and UFW_SCREEN_B via XOR with UFW_SCREEN_XOR.
    move.l  #UFW_SCREEN_A, UFW_SCREEN_PAGE

    ; The frame now in the cart framebuffer counts as seen: the first blit
    ; waits for the RP's next publish (see FB_FRAME_COUNTER_ADDR).
    move.w  FB_FRAME_COUNTER_ADDR, UFW_LAST_FRAME

    ; Shifter base HIGH byte ($06) is the same for both screen pages
    ; (UFW_SCREEN_A and UFW_SCREEN_B), so we write it ONCE here and only
    ; update the MID byte per VBL in userfw_after_copy below (saves ~20 cyc/VBL).
    move.b  #(UFW_SCREEN_A >> 16), VIDEO_BASE_ADDR_HIGH.w

    ; Pin IRQ state for the duration of userfw_vbl_loop:
    ;   SR = $2300: supervisor mode, IPL=3. Blocks levels 1-3 (HBL
    ;   at IPL 2, which only the border handlers let through), allows VBL
    ;   at IPL 4 and the MFP's at IPL 6.
    move.w  #$2300, sr

    ; --- Per-VBL loop ---
    moveq   #IDLE_NO_FRAME_ITERS, d7
userfw_vbl_loop:
    ; Wait for the next vsync: userfw_vbl clears UFW_VBL_FLAG. While the YM
    ; plays at 50 fps, the wait writes the samples at their pace (D7: the
    ; iterations before the first, set by whoever comes here; see "Samples
    ; by the copy"); otherwise `stop #$2300` halts until an IRQ at level > 3
    ; (the VBL at IPL 4, the MFP's at 6), and a wake by another IRQ stops
    ; again.
    move.w  #-1, UFW_VBL_FLAG
    ifne    FB_INLINE_SAMPLES
    tst.w   UFW_YM_SAMPLES
    beq.s   .wait_vbl
    lea     YM_SELECT.w, a4
    move.w  d7, d0
.idle_delay:
    tst.w   UFW_VBL_FLAG
    beq.s   .vbl_wake
    dbf     d0, .idle_delay
    SAMPLE_WRITE d1
    moveq   #IDLE_SAMPLE_ITERS, d0
    bra.s   .idle_delay
    endc
.wait_vbl:
    stop    #$2300
    tst.w   UFW_VBL_FLAG
    bne.s   .wait_vbl
.vbl_wake:
    HANDLER_SAMPLE
    STUDY_POINT STUDY_POINT_WAKE

    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_VSYNC, PALETTE_IDX0.w   ; border = vsync mark
    endc

    ; DMA sound (see AUDIO_BUFFER_ADDR): where the chip plays now, by
    ; AUDIO_POS_UNIT bytes, for the RP; then the mirror copied into the
    ; ring from where the last copy ended up to the profile's lead
    ; (UFW_DMA_LEAD) ahead of the chip. The counter moves while it is read:
    ; its middle byte is read again. A frontier out of step (the first VBL,
    ; a long stall) copies the lead. About 0.17 ms at 12,517 Hz, 0.35 at
    ; 25,033 Hz; Timer-D's samples, off on this path, take 2.1 ms at 5,585 Hz.
    tst.w   UFW_AUDIO_DMA
    beq     .dma_done
.dma_pos:
    moveq   #0, d0
    move.b  DMA_SND_COUNT_MID.w, d0
    move.b  DMA_SND_COUNT_LOW.w, d1
    cmp.b   DMA_SND_COUNT_MID.w, d0
    bne.s   .dma_pos
    lsl.w   #8, d0
    move.b  d1, d0
    sub.w   #(UFW_DMA_RING & $FFFF), d0
    and.w   #(AUDIO_DMA_RING_BYTES - AUDIO_POS_UNIT), d0
    move.w  d0, d1
    lsr.w   #4, d1                        ; / AUDIO_POS_UNIT
    lea     AUDIO_POS_WINDOW, a1
    tst.b   (a1, d1.w)
    add.w   UFW_DMA_LEAD, d0
    and.w   #(AUDIO_DMA_RING_BYTES - 1), d0 ; copy up to here
    move.w  UFW_DMA_FRONT, d1             ; from where the last copy ended
    move.w  d0, UFW_DMA_FRONT
    move.w  d0, d2
    sub.w   d1, d2
    and.w   #(AUDIO_DMA_RING_BYTES - 1), d2
    cmp.w   #(AUDIO_DMA_RING_BYTES / 2), d2
    bls.s   .dma_count
    move.w  d0, d1
    sub.w   UFW_DMA_LEAD, d1
    and.w   #(AUDIO_DMA_RING_BYTES - 1), d1
    move.w  UFW_DMA_LEAD, d2
    ; D1 = from, D2 = bytes, both multiples of AUDIO_POS_UNIT: the run up
    ; to the ring's end, then what is left from its start, in MOVEMs of 32
    ; bytes (166 cycles each: 5.2 cycles a byte against the 11 of a move.l
    ; with indexed addressing and a mask per 8 bytes).
.dma_count:
    tst.w   d2
    beq.s   .dma_done
    move.w  #AUDIO_DMA_RING_BYTES, d3
    sub.w   d1, d3                        ; bytes from `from` to the ring's end
    cmp.w   d2, d3
    bls.s   .dma_wrapped
    move.w  d2, d3                        ; all of it before the end
.dma_wrapped:
    sub.w   d3, d2                        ; what goes round: 0 or more
    lea     AUDIO_BUFFER_ADDR, a1
    adda.w  d1, a1
    lea     UFW_DMA_RING, a2
    adda.w  d1, a2
    bsr.s   .dma_run
    tst.w   d2
    beq.s   .dma_done
    move.w  d2, d3
    lea     AUDIO_BUFFER_ADDR, a1
    lea     UFW_DMA_RING, a2
    bsr.s   .dma_run
    bra.s   .dma_done
    ; D3 bytes, a multiple of 16, from (a1) to (a2): an odd 16-byte unit
    ; first, then pairs. Clobbers D0, D1, D3-D7, A1-A4.
.dma_run:
    lsr.w   #5, d3                        ; pairs; the carry: an odd unit
    bcc.s   .dma_run_pairs
    movem.l (a1)+, d4-d7
    movem.l d4-d7, (a2)
    lea     16(a2), a2
.dma_run_pairs:
    subq.w  #1, d3
    bmi.s   .dma_run_done
.dma_run_loop:
    movem.l (a1)+, d0-d1/d4-d7/a3-a4
    movem.l d0-d1/d4-d7/a3-a4, (a2)
    lea     32(a2), a2
    dbf     d3, .dma_run_loop
.dma_run_done:
    rts
.dma_done:

    ; Publish RP-supplied palette to the shifter. 16 words
    ; from PALETTE_ADDR -> $FFFF8240..$FFFF825E via two MOVEMs.
    ; Cost: 76 (load) + 72 (store) + 16 (lea) = ~164 cyc / VBL =
    ; ~20 us. Apps that don't want RP-driven palette can leave the
    ; cart slot zero (= all-black screen, since the m68k still
    ; publishes it every frame) -- swap the load EA below for
    ; their own palette source if needed.
    lea     PALETTE_ADDR, a5
    movem.l (a5), d0-d7
    movem.l d0-d7, PALETTE_BASE.w

    ; Blit only a frame the RP has finished publishing, and only once
    ; (see FB_FRAME_COUNTER_ADDR), and no sooner than the profile's VBLs a
    ; frame after the last one (PROFILE_25FPS: every second VBL, on every
    ; machine; a frame late by a VBL delays only the next one). Nothing
    ; new: no blit, no flip, no ack.
    move.w  UFW_VBL_COUNT, d1
    sub.w   UFW_FRAME_VBL, d1
    cmp.w   UFW_VBLS_A_FRAME, d1
    blo     userfw_no_frame
    move.w  FB_FRAME_COUNTER_ADDR, d0
    cmp.w   UFW_LAST_FRAME, d0
    beq     userfw_no_frame
    move.w  d0, UFW_LAST_FRAME
    move.w  UFW_VBL_COUNT, UFW_FRAME_VBL
    STUDY_POINT STUDY_POINT_COPY

    ; The lines to copy (COPY_LINES_ADDR): when the word changes, clear the
    ; image's lines past the short count in both pages, once (about 2.5 ms),
    ; so that a short copy leaves black below it, not a frame before.
    move.w  COPY_LINES_ADDR, d0
    cmp.w   UFW_COPY_LINES, d0
    beq.s   .lines_same
    move.w  d0, UFW_COPY_LINES
    bsr     userfw_clear_bottom
.lines_same:

    ; The copy: the blitter when there is one and the RP asks for it, or
    ; leaves it to us on the DMA sound path (BLIT_MODE_ADDR), else the 68000
    ; (FBDRV_INLINE). The RP published the frame in the copier's layout, by
    ; the same rule.
    movea.l UFW_SCREEN_PAGE, a5
    tst.w   UFW_HAS_BLITTER
    beq     .copy_cpu
    move.w  BLIT_MODE_ADDR, d1
    cmp.b   #BLIT_MODE_BLITTER, d1
    beq.s   .copy_blitter
    tst.b   d1
    bne     .copy_cpu
    tst.w   UFW_AUDIO_DMA
    beq     .copy_cpu
.copy_blitter:
    lea     BLT_BASE.w, a1
    move.w  #2, BLT_SRC_X_INC-BLT_BASE(a1)
    move.w  #2, BLT_SRC_Y_INC-BLT_BASE(a1)
    move.l  #FRAMEBUFFER_ADDR, BLT_SRC_ADDR-BLT_BASE(a1)
    moveq   #-1, d2
    move.w  d2, BLT_ENDMASK1-BLT_BASE(a1)
    move.w  d2, BLT_ENDMASK2-BLT_BASE(a1)
    move.w  d2, BLT_ENDMASK3-BLT_BASE(a1)
    move.w  #2, BLT_DST_X_INC-BLT_BASE(a1)
    move.w  #BLT_DST_Y_INC, BLT_DST_Y_INC_REG-BLT_BASE(a1)
    lea     (UFW_IMAGE_LINE * FB_ROW_BYTES)(a5), a2
    move.l  a2, BLT_DST_ADDR-BLT_BASE(a1)
    move.w  #FB_LINE_BYTES / 2, BLT_X_COUNT-BLT_BASE(a1)
    move.w  #BLT_HOP_OP_COPY, BLT_HOP-BLT_BASE(a1)
    clr.b   BLT_SKEW-BLT_BASE(a1)
    lsr.w   #8, d1                        ; lines a piece: the mode's high byte
    bne.s   .blit_pieces
    moveq   #BLIT_PIECE_DEFAULT, d1
.blit_pieces:
    move.w  COPY_LINES_ADDR, d2           ; lines left: the frame's top ones
    bne.s   .blit_lines                   ; (0: all of them)
    move.w  #FB_COPY_LINES, d2
.blit_lines:

    ; A piece is at most the lines before the next border handler must run
    ; (a switch needs the CPU within a line, and the blitter holds it): while
    ; Timer-C runs, its count in lines less a margin; then while Timer-B
    ; counts the display lines, its count less a margin; after the bottom
    ; switch, the rest. None left: wait for the handler (any interrupt ends
    ; the stop; the loop looks again).
.blit_next:
    move.b  MFP_TCDCR.w, d0
    andi.w  #$70, d0
    beq.s   .blit_timerb
    moveq   #0, d0
    move.b  MFP_TCDR.w, d0
    mulu    #TIMERC_LINES_A_TICK_256, d0
    lsr.w   #8, d0
    subq.w  #BLIT_MARGIN_TIMERC, d0
    bra.s   .blit_size
.blit_timerb:
    tst.b   MFP_TBCR.w
    beq.s   .blit_rest
    moveq   #0, d0
    move.b  MFP_TBDR.w, d0
    subq.w  #BLIT_MARGIN_TIMERB, d0
    bra.s   .blit_size
.blit_rest:
    move.w  d2, d0
.blit_size:
    tst.w   d0
    ble.s   .blit_wait
    cmp.w   d1, d0
    bls.s   .blit_fit
    move.w  d1, d0
.blit_fit:
    cmp.w   d2, d0
    bls.s   .blit_go
    move.w  d2, d0
.blit_go:
    move.w  d0, BLT_Y_COUNT-BLT_BASE(a1)
    move.b  #BLT_BUSY_HOG, BLT_CTRL-BLT_BASE(a1)
    nop
    sub.w   d0, d2
    bne.s   .blit_next
    bra     userfw_after_copy
.blit_wait:
    stop    #$2300
    bra.s   .blit_next

    ; The 68000's copy (FBDRV_INLINE): from the image's last line in the
    ; hidden page to its first.
.copy_cpu:
    adda.l  #UFW_COPY_START, a5
    movea.l UFW_SAMPLE_PORT, a4           ; the samples' writes (FBDRV_PAIR_SAMPLE)
    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_RUNNING, PALETTE_IDX0.w  ; border = white (blit in flight)
    endc
    ifne    FB_INLINE_SAMPLES
    SAMPLE_WRITE d0                       ; (to the sink when nothing plays)
    endc
    cmpi.w  #FB_COPY_LINES_SHORT, COPY_LINES_ADDR
    bne.s   .copy_all
    ; The top FB_COPY_LINES_SHORT lines only: the framebuffer holds the
    ; lines from the last to the first, so skip the bottom ones.
    movea.l #FRAMEBUFFER_ADDR + (FB_COPY_LINES - FB_COPY_LINES_SHORT) * FB_LINE_BYTES, a6
    suba.l  #(FB_COPY_LINES - FB_COPY_LINES_SHORT) * FB_ROW_BYTES, a5
    jmp     fbdrv_short(pc)
.copy_all:
    FBDRV_INLINE                      ; inline cart->ST screen copy
    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_DONE, PALETTE_IDX0.w     ; border = green (copy done)
    endc

userfw_after_copy:
    STUDY_POINT STUDY_POINT_COPIED

    ; Flip the video base to the just-written page. Only the MID byte
    ; of the screen base differs between the two pages -- HIGH was
    ; written once at boot (constant $06 for both pages).
    ;
    ; UFW_SCREEN_PAGE is a 32-bit address stored big-endian, so byte
    ; +2 of the longword is exactly the MID byte (bits 8..15) we need
    ; to write to VIDEO_BASE_ADDR_MID. Read it straight from memory
    ; instead of recomputing via lsr/move chain from A5.
    move.b  UFW_SCREEN_PAGE+2, VIDEO_BASE_ADDR_MID.w

    ; Toggle UFW_SCREEN_PAGE between SCREEN_A and SCREEN_B for the
    ; next frame.
    move.l  UFW_SCREEN_PAGE, d0
    eor.l   #UFW_SCREEN_XOR, d0
    move.l  d0, UFW_SCREEN_PAGE

    ; Frame-sync ack: one cart-bus read tells the RP the blit
    ; is finished and the cart FB is free to overwrite. Emitted every
    ; VBL (the FB is free here -- blit done, page flipped). The RP's
    ; commemul ring captures the read; fb_publish() on the RP blocks
    ; until it sees this before running the next chunky-to-planar.
    tst.b   VBLSYNC_ADDR
    moveq   #IDLE_FIRST_ITERS, d7         ; the idle samples' first delay
    bra.s   userfw_input_check
userfw_no_frame:
    moveq   #IDLE_NO_FRAME_ITERS, d7

userfw_input_check:
    ; IKBD commands from the RP (IKBD_OUT_ADDR): when their generation
    ; changes, send the bytes one per VBL, only when the ACIA can take one
    ; (no waiting), a 0 as a VBL with no byte; then report the generation.
    ; A new generation before the end starts over. Nothing is sent while the
    ; RP rewrites the block (busy bit), and a byte only if the generation is
    ; still the one read before it.
    move.w  IKBD_OUT_GEN, d0
    btst    #IKBD_OUT_BUSY_BIT, d0
    bne.s   .ikbd_out_done
    cmp.w   UFW_IKBD_OUT_GEN, d0
    beq.s   .ikbd_out_next
    move.w  d0, UFW_IKBD_OUT_GEN
    clr.w   UFW_IKBD_OUT_STEP
.ikbd_out_next:
    move.w  UFW_IKBD_OUT_STEP, d1
    bmi.s   .ikbd_out_done                ; all sent and reported
    move.w  IKBD_OUT_LEN, d2
    cmp.w   #IKBD_OUT_MAX, d2
    bls.s   .ikbd_out_len
    moveq   #IKBD_OUT_MAX, d2
.ikbd_out_len:
    cmp.w   d2, d1
    bhs.s   .ikbd_out_report
    btst    #1, ACIA_KBD_STATUS.w         ; can the ACIA take a byte?
    beq.s   .ikbd_out_done                ; not yet: next VBL
    lea     IKBD_OUT_BYTES, a1
    move.b  (a1, d1.w), d2
    cmp.w   IKBD_OUT_GEN, d0              ; still the same commands?
    bne.s   .ikbd_out_done
    tst.b   d2
    beq.s   .ikbd_out_sent                ; 0: nothing this VBL
    move.b  d2, ACIA_KBD_DATA.w
.ikbd_out_sent:
    addq.w  #1, UFW_IKBD_OUT_STEP
    bra.s   .ikbd_out_done
.ikbd_out_report:
    move.w  #-1, UFW_IKBD_OUT_STEP
    and.w   #$FF, d0
    lea     IKBD_OUT_WINDOW, a1
    tst.b   (a1, d0.w)
.ikbd_out_done:

    ; ESC detection: the RP-side IKBD demux writes
    ; CMD_BOOT_GEM into CMD_MAGIC_SENTINEL_ADDR on ESC press. CMD_RESET:
    ; the RP is about to reboot into Booster. Any other
    ; sentinel value (NOP, future commands) leaves the loop running.
    STUDY_POINT STUDY_POINT_IDLE
    move.l  CMD_MAGIC_SENTINEL_ADDR, d0
    cmp.l   #CMD_RESET, d0
    beq     .cold_reset
    cmp.l   #CMD_BOOT_GEM, d0
    bne     userfw_vbl_loop

    ; --- ESC pressed: restore IRQ state and return to TOS ---------
    ;
    ; Mask interrupts before touching MFP / vectors.
    ori.w   #$0700, sr

    ; The DMA sound chip stops; the Microwire stays as TOS sets it.
    tst.w   UFW_AUDIO_DMA
    beq.s   .exit_sound
    clr.b   DMA_SND_CTRL.w
.exit_sound:

    ; The save-area pointer again, in case anything clobbered A5 during
    ; the run.
    lea     UFW_SAVE_AREA, a5

    ; Stop the timers so no IRQ can fire mid-restore; then TOS's Timer-C
    ; and Timer-D back as they were, 50 Hz, and a Mega STE's speed.
    clr.b   MFP_TBCR.w
    clr.b   MFP_TACR.w
    clr.b   MFP_TCDCR.w
    move.b  30(a5), MFP_TCDR.w
    move.b  31(a5), MFP_TDDR.w
    move.b  29(a5), MFP_TCDCR.w
    move.b  #SYNC_50HZ, VIDEO_SYNC.w
    cmpi.w  #MACHINE_MEGA_STE, UFW_MACHINE
    bne.s   .exit_mste
    move.b  UFW_MSTE_CTRL+1, MSTE_CTRL.w
.exit_mste:

    ; Restore MFP IER / IMR.
    move.b  24(a5), MFP_IERA.w
    move.b  25(a5), MFP_IERB.w
    move.b  26(a5), MFP_IMRA.w
    move.b  27(a5), MFP_IMRB.w
    move.b  28(a5), MFP_VR.w             ; restore TOS's S=1 / vector base

    ; Restore the 6 vectors we overwrote.
    move.l  0(a5), VEC_HBL.w
    move.l  4(a5), VEC_TIMERD.w
    move.l  8(a5), VEC_TIMERC.w
    move.l  12(a5), VEC_ACIA.w
    move.l  16(a5), VEC_TIMERB.w
    move.l  20(a5), VEC_TIMERA.w

    ; Restore TOS's VBL vector ($70 save from UFW_VBL_VEC_SAVE).
    move.l  UFW_VBL_VEC_SAVE, VBL_VECTOR.w

    ; Give TOS its mouse and joysticks back (see IKBD_CMD_RESET_HDR).
    IKBD_SEND IKBD_CMD_JOY_EVENTS
    IKBD_SEND IKBD_CMD_MOUSE_REL
    move.b  #ACIA_MIDI_CTRL_TOS, ACIA_MIDI_CTRL.w

    ; Restore SR to TOS's usual IPL=3 (matches md-oric main.s:230).
    ; From here on TOS handles HBL / Timer / ACIA again -- IKBD will
    ; re-pump GEMDOS's keyboard buffer, GEM mouse cursor revives, etc.
    move.w  #$2300, sr

    ; Restore screen base via XBIOS Setscreen.
    move.w  #-1, -(sp)                ; no rez change
    move.l  UFW_PHYSBASE_SAVE, -(sp)  ; physical screen
    move.l  UFW_PHYSBASE_SAVE, -(sp)  ; logical screen
    move.w  #5, -(sp)                 ; XBIOS Setscreen
    trap    #14
    lea     12(sp), sp
    rts

    ; --- CMD_RESET: cold-reset the ST, from RAM -------------------
    ;
    ; The RP reboots into Booster about 100 ms after it asks for this,
    ; and from then on the cartridge answers nothing useful: nothing may
    ; run from it. Mask every interrupt (userfw's handlers live in the
    ; cartridge), copy userfw_reset_stub to UFW_RESET_STUB and run it
    ; there. TOS's cold boot reinitialises the shifter, MFP, IKBD, YM
    ; and every vector userfw took, so nothing is restored by hand.
.cold_reset:
    move.w  #$2700, sr
    tst.w   UFW_AUDIO_DMA
    beq.s   .reset_sound
    clr.b   DMA_SND_CTRL.w                ; nothing plays from a ring TOS reuses
.reset_sound:
    lea     userfw_reset_stub(pc), a1
    lea     UFW_RESET_STUB, a2
    moveq   #((userfw_reset_stub_end - userfw_reset_stub) / 2) - 1, d0
.copy_reset_stub:
    move.w  (a1)+, (a2)+
    dbf     d0, .copy_reset_stub
    jmp     UFW_RESET_STUB

; -------------------------------------------------------------------
; userfw_vbl -- VBL interrupt handler. Four jobs:
;   1. Tell the RP how many IKBD bytes have been read so far, low byte
;      (IKBD_COUNT_WINDOW): the RP checks it got every one, and uses the
;      report as a clock among the IKBD bytes.
;   2. The samples' cursor A0 (the YM path): past the ring's end, in the
;      spill that mirrors the ring's start, bring it back by the ring's
;      size, and tell the RP where it is (AUDIO_POS_WINDOW, by
;      AUDIO_POS_UNIT bytes). Wrapping once per VBL keeps the `cmpa.l +
;      bcs.s` out of every sample write. On the other paths A0 goes back
;      to the zero area. A0 is dedicated to audio (excluded from the
;      FBDRV_INLINE MOVEM list and from the ACIA handler), so it's safe to
;      move here from IRQ context.
;   3. Clear UFW_VBL_FLAG so userfw_vbl_loop's `stop`-then-check wait can
;      distinguish a VBL wake from a Timer-B (or other MFP) wake.
;   4. The borders (see "Overscan"): report which ones the frame that ended
;      had open, note the page this frame shows, and when no chain of
;      borders runs, arm Timer-B for this frame's bottom one.
;
; This replaces TOS's VBL handler entirely while userfw is running,
; so mouse / cursor-blink / keyboard-repeat / _vblqueue all stop
; firing. The ACIA IRQ ($118) is userfw's own (userfw_acia_irq), so
; GEMDOS's keyboard buffer is no longer filled; the keys go to the RP.
userfw_vbl:
    ; Timer-D (IPL 6, the 25 fps build) can interrupt this handler (IPL 4),
    ; and it reads a sample through A0: masked here, it never sees A0 half
    ; moved. This handler interrupts the blit between a MOVEM load and its
    ; store, when every register but A0 and A7 holds pixels: D0 and A1 are
    ; saved. About 30 us per VBL. userfw_acia_irq cannot run in the middle
    ; of the count report either, so every byte it counted was forwarded
    ; before the report.
    move.w  #$2700, sr
    addq.w  #1, UFW_VBL_COUNT
    HANDLER_SAMPLE                        ; the gap around the VBL: a sample at
    ifne    TIME_STUDY                    ; the entry, one at the end, one at
    movem.l d0-d2/a1, -(sp)               ; the wake, one as the copy starts
    moveq   #STUDY_POINT_VBL, d2
    bsr     study_report
    tst.w   UFW_YM_SAMPLES
    beq.s   .vbl_no_count
    move.l  a0, d0                        ; the cursor only grows between VBLs
    sub.l   UFW_SAMPLE_LAST, d0
    ifne    FB_INLINE_SAMPLES
    divu.w  #6, d0                        ; six bytes a sample
    swap    d0
    clr.w   d0                            ; not the remainder
    swap    d0
    else
    lsr.w   #1, d0                        ; two
    endc
    moveq   #STUDY_POINT_SAMPLES, d2
    bsr     study_send
.vbl_no_count:
    movem.l (sp)+, d0-d2/a1
    endc
    movem.l d0/a1, -(sp)
    move.w  UFW_BORDERS, d0
    movea.l #BORDER_WINDOW, a1
    tst.b   (a1, d0.w)
    clr.w   UFW_BORDERS
    moveq   #0, d0
    move.b  VIDEO_BASE_ADDR_MID.w, d0     ; the page the shifter shows now
    lsl.w   #8, d0
    move.w  d0, UFW_SHOWN_BASE
    tst.w   UFW_TOP_ARMED
    bne.s   .vbl_chained                  ; Timer-C opens the top, then arms Timer-B
    move.w  #BOTTOM_EXPECT_CLOSED, UFW_BOTTOM_EXPECT
    clr.b   MFP_TBCR.w
    move.b  #TIMERB_LINES_CLOSED, MFP_TBDR.w
    move.b  #TBCR_EVENT, MFP_TBCR.w
.vbl_chained:
    clr.w   UFW_TOP_ARMED
    moveq   #0, d0
    move.b  UFW_IKBD_COUNT+1, d0
    movea.l #IKBD_COUNT_WINDOW, a1
    tst.b   (a1, d0.w)
    ; The samples' cursor: on the YM path it runs on through the ring;
    ; past the ring's end (in the spill, which mirrors the ring's start)
    ; it comes back by the ring's size, and the RP learns where it is, by
    ; AUDIO_POS_UNIT bytes (16: a shift). Elsewhere, back to the zero area.
    tst.w   UFW_YM_SAMPLES
    beq.s   .vbl_a0_zero
    cmpa.l  #AUDIO_BUFFER_ADDR + AUDIO_YM_RING_BYTES, a0
    blo.s   .vbl_a0_in
    suba.l  #AUDIO_YM_RING_BYTES, a0
.vbl_a0_in:
    ifne    TIME_STUDY
    move.l  a0, UFW_SAMPLE_LAST
    endc
    move.l  a0, d0
    sub.l   #AUDIO_BUFFER_ADDR, d0
    lsr.w   #4, d0
    movea.l #AUDIO_POS_WINDOW, a1
    tst.b   (a1, d0.w)
    bra.s   .vbl_a0_done
.vbl_a0_zero:
    movea.l UFW_SAMPLE_BASE, a0           ; zeros: the writes play nothing
.vbl_a0_done:
    movem.l (sp)+, d0/a1
    HANDLER_SAMPLE
    clr.w   UFW_VBL_FLAG
    rte

; -------------------------------------------------------------------
; userfw_timerd_audio -- Timer-D IRQ handler, the YM path's samples in the
; 25 fps build (at 50 fps the copy writes them: "Samples by the copy").
; Fires at the profile's rate (Timer-D in /4 delay mode, TDDR = the
; profile's count: 21,943 Hz).
;
; Dual-channel Ghostbusters-LUT mode: each sample in the cart buffer
; is 2 bytes = (vA, vB), pre-resolved at build time by running the
; raw G1.SAM bytes through the demo's 64-entry SAMPLE1 LUT (top 6
; bits of each PCM byte index a (chA, chB) pair). Per fire:
;   1. Write vA to YM ch A vol (reg 8 latched on entry).
;   2. Latch reg 9 (ch B vol).
;   3. Write vB to YM ch B vol.
;   4. Re-latch reg 8 so the next fire writes ch A immediately.
;
; A0 is a DEDICATED cart audio-buffer cursor (postinc walks 2 bytes a
; fire through the ring; userfw_vbl wraps it and reports it).
;
; MFP is in auto-EOI mode (VR S=0) so the in-service bit clears
; automatically on each IACK cycle.
;
; Cycle budget per fire:
;   move.b  (a0)+, YM_DATA.w               ; 12 cyc -- vA -> ch A vol
;   move.b  #YM_REG_CHB_VOL, YM_SELECT.w   ; 12 cyc -- latch reg 9
;   move.b  (a0)+, YM_DATA.w               ; 12 cyc -- vB -> ch B vol
;   move.b  #YM_REG_CHA_VOL, YM_SELECT.w   ; 12 cyc -- re-latch reg 8
;   rte                                    ; 20 cyc
;   ---                                    ; 68 cyc/IRQ
; Plus ~44 cyc of IRQ entry and exit: ~112 cyc per fire, 112 fires
; per PAL VBL = ~12.5 k cyc = ~1.6 ms = ~8% of the frame.
userfw_timerd_audio:
    move.b  (a0)+, YM_DATA.w               ; vA -> ch A vol
    move.b  #YM_REG_CHB_VOL, YM_SELECT.w   ; latch ch B vol reg
    move.b  (a0)+, YM_DATA.w               ; vB -> ch B vol
    move.b  #YM_REG_CHA_VOL, YM_SELECT.w   ; back to ch A vol reg for next fire
    rte

; -------------------------------------------------------------------
; BORDER_SWITCH n -- the 60 Hz write n NOPs from here and the 50 Hz one
; BORDER_HOLD_NOPS after it (see "Overscan"). Interrupts masked.
BORDER_SWITCH         macro
    move.w  #$2700, sr
    rept    \1
    nop
    endr
    move.b  #SYNC_60HZ, VIDEO_SYNC.w
    rept    BORDER_HOLD_NOPS
    nop
    endr
    move.b  #SYNC_50HZ, VIDEO_SYNC.w
                      endm

; BORDER_QUIET / BORDER_LOUD: the interrupts that would move a switch wait
; while a border handler does (the ACIA's, the samples', the stopwatch's):
; masked, they stay pending and fire after.
BORDER_QUIET          macro
    bclr    #MFP_GPIP4_BIT, MFP_IMRB.w
    bclr    #MFP_TIMERD_BIT, MFP_IMRB.w
    ifne    TIME_STUDY
    bclr    #MFP_TIMERA_BIT, MFP_IMRA.w
    endc
                      endm
BORDER_LOUD           macro
    bset    #MFP_GPIP4_BIT, MFP_IMRB.w
    tst.w   UFW_AUDIO_DMA
    bne.s   .\@dma
    bset    #MFP_TIMERD_BIT, MFP_IMRB.w
.\@dma:
    ifne    TIME_STUDY
    bset    #MFP_TIMERA_BIT, MFP_IMRA.w
    endc
                      endm

; -------------------------------------------------------------------
; userfw_top_border -- Timer-C, started by the last frame's bottom border to
; fire early in this frame's line 30. Waits for line starts until Timer-C's
; count, which runs on, says line 33 has started (the video counter cannot
; tell: the shifter fetches nothing in the top border), opens the top
; border, and arms Timer-B for the bottom one: the display lines from line
; 34 to the end of line 259. A wake on line 34 or later (an entry delayed
; past a line) leaves the top closed and arms Timer-B from line 63, so the
; bottom still opens. Saves what it uses: it fires between a MOVEM load and
; its store.
userfw_top_border:
    movem.l d0-d1, -(sp)
    BORDER_QUIET
    moveq   #TOP_LINE_TRIES - 1, d1
    move.w  #$2100, sr                    ; the HBL pending since long: at once
.top_line:
    stop    #$2100                        ; a line starts
    move.w  #TIMERC_TOP_COUNT, d0
    sub.b   MFP_TCDR.w, d0                ; ticks since Timer-C fired
    cmp.w   #TOP_WAKE_TICKS_33, d0
    dbcc    d1, .top_line                 ; fewer: line 32 or before
    bcs     .top_missed                   ; tried them all before line 33
    cmp.w   #TOP_WAKE_TICKS_34, d0
    bcc     .top_missed                   ; line 34 or later
    BORDER_SWITCH TOP_DELAY_NOPS
    clr.b   MFP_TBCR.w
    move.b  #TIMERB_LINES_OPEN, MFP_TBDR.w
    move.b  #TBCR_EVENT, MFP_TBCR.w
    move.w  #BOTTOM_EXPECT_OPEN, UFW_BOTTOM_EXPECT
    ori.w   #BORDER_TOP, UFW_BORDERS
    bra.s   .top_done
.top_missed:
    clr.b   MFP_TBCR.w
    move.b  #TIMERB_LINES_CLOSED, MFP_TBDR.w
    move.b  #TBCR_EVENT, MFP_TBCR.w
    move.w  #BOTTOM_EXPECT_CLOSED, UFW_BOTTOM_EXPECT
.top_done:
    andi.b  #$07, MFP_TCDCR.w             ; Timer-C stops; Timer-D runs on
    ifne    TIME_STUDY
    movem.l d2/a1, -(sp)
    moveq   #STUDY_POINT_TOPWAKE, d2      ; D0: the count the switch was made at
    bsr     study_send
    movem.l (sp)+, d2/a1
    endc
    HANDLER_SAMPLE                        ; the sample the wait delayed
    BORDER_LOUD
    movem.l (sp)+, d0-d1
    rte

; -------------------------------------------------------------------
; userfw_bottom_border -- Timer-B in event count mode, near the end of line
; 259. Waits for line starts (261's, then 262's) until the video counter
; says line 262 has started (within its 160 bytes from UFW_BOTTOM_EXPECT
; bytes into the page shown: by the time it is read, the line's display has
; begun), opens the bottom border, and starts Timer-C for the next frame's
; top border. A counter that never matches (the top border closed when it
; should have opened, or an entry so late that the wait skipped line 262)
; leaves both closed: userfw_vbl starts the chain again. Saves what it
; uses: it fires between a MOVEM load and its store.
userfw_bottom_border:
    clr.b   MFP_TBCR.w                    ; once a frame
    movem.l d0-d1/a1, -(sp)
    BORDER_QUIET
    lea     VIDEO_REGS.w, a1
    moveq   #BOTTOM_LINE_TRIES - 1, d1
    move.w  #$2100, sr                    ; the HBL pending since long: at once
.bottom_line:
    stop    #$2100                        ; a line starts
    movep.w 7(a1), d0                     ; the video counter, middle and low bytes
    sub.w   UFW_SHOWN_BASE, d0
    sub.w   UFW_BOTTOM_EXPECT, d0         ; bytes into line 262: the display has
    cmp.w   #FB_ROW_BYTES, d0             ; started by now, at cycle 56
    dbcs    d1, .bottom_line
    bcc     .bottom_missed                ; tried them all: no line 262
    BORDER_SWITCH BOTTOM_DELAY_NOPS
    move.b  #TIMERC_TOP_COUNT, MFP_TCDR.w ; Timer-C stopped: this loads it
    move.b  MFP_TCDCR.w, d0
    andi.b  #$07, d0
    ori.b   #(TIMERC_TOP_PRESCALER << 4), d0
    move.b  d0, MFP_TCDCR.w               ; starts
    move.w  #-1, UFW_TOP_ARMED
    ori.w   #BORDER_BOTTOM, UFW_BORDERS
.bottom_missed:
    ifne    TIME_STUDY
    movem.l d2/a1, -(sp)
    moveq   #STUDY_POINT_BOTWAKE, d2      ; D0: the counter's offset at the last wake
    bsr     study_send
    movem.l (sp)+, d2/a1
    endc
    HANDLER_SAMPLE                        ; the sample the wait delayed
    BORDER_LOUD
    movem.l (sp)+, d0-d1/a1
    rte

; -------------------------------------------------------------------
; userfw_acia_irq -- keyboard ACIA receive interrupt (MFP GPIP4, $118).
; Forwards the byte the ACIA holds with one cart-bus read, counts it
; (UFW_IKBD_COUNT, see userfw_vbl) and returns. One byte per interrupt:
; the 6850 holds a single byte (a second one overruns it, reported below),
; reading it raises the ACIA's IRQ line, and the next byte lowers it again,
; an edge the MFP sees. The handler is kept short because it is paid for
; every byte: at line speed 16 times a VBL, when the full-screen blit
; leaves almost no slack. It can fire between a MOVEM load and its store
; in FBDRV_INLINE: it saves D0 and A1 and never touches A0 (the audio
; cursor). Auto-EOI: no in-service bit to clear.
userfw_acia_irq:
    move.l  d0, -(sp)
    move.l  a1, -(sp)
    moveq   #0, d0
    move.b  ACIA_KBD_STATUS.w, d0
    btst    #0, d0                        ; a byte waiting?
    beq.s   .acia_done
    btst    #5, d0                        ; overrun: bytes were lost before it
    beq.s   .acia_read
    tst.b   IKBD_OVERRUN_ADDR
.acia_read:
    move.b  ACIA_KBD_DATA.w, d0           ; clears RX-ready and the overrun
    lea     IKBD_WINDOW_BASE, a1
    tst.b   (a1, d0.w)                    ; forward it
    addq.w  #1, UFW_IKBD_COUNT
.acia_done:
    move.l  (sp)+, a1
    move.l  (sp)+, d0
    rte

; -------------------------------------------------------------------
; userfw_dummy_irq -- single-rte IRQ handler for vectors we want to
; silence (HBL $68, Timer-A $134; Timer-C $114, Timer-D $110, ACIA
; $118 and Timer-B $120 too, until their own handlers go in). The border
; handlers wait for an HBL with `stop`: its handler only returns. Stopping TOS's handlers cuts the per-frame jitter they
; impose on the blit; we don't need their behaviour because the
; framebuffer template owns the screen + IKBD until ESC exit.
; -------------------------------------------------------------------
; userfw_clear_bottom -- clears the image's lines from FB_COPY_LINES_SHORT
; to the last in both screen pages (their first FB_LINE_BYTES bytes, the
; image's), what a short copy leaves alone. Clobbers D0-D1, A1.
userfw_clear_bottom:
    lea     UFW_SCREEN_A + (UFW_IMAGE_LINE + FB_COPY_LINES_SHORT) * FB_ROW_BYTES, a1
    bsr.s   .clear_lines
    lea     UFW_SCREEN_B + (UFW_IMAGE_LINE + FB_COPY_LINES_SHORT) * FB_ROW_BYTES, a1
.clear_lines:
    move.w  #FB_COPY_LINES - FB_COPY_LINES_SHORT - 1, d1
.clear_line:
    moveq   #FB_LINE_BYTES / 4 - 1, d0
.clear_long:
    clr.l   (a1)+
    dbf     d0, .clear_long
    lea     (FB_ROW_BYTES - FB_LINE_BYTES)(a1), a1
    dbf     d1, .clear_line
    rts

userfw_dummy_irq:
    rte

    ifne    TIME_STUDY
; -------------------------------------------------------------------
; userfw_timera_tick -- the stopwatch's wrap (TIME_STUDY): every 256
; ticks of Timer-A, 1.04 ms.
userfw_timera_tick:
    addq.w  #1, UFW_SW_WRAPS
    rte

; study_report -- tells the RP point D2 and the stopwatch's low 16 bits
; (TIME_STUDY): three ROM3 reads, with the interrupts masked so that no other
; report (the VBL's) falls between them. study_send: point D2 with the word
; D0 in place of the ticks. Clobbers D0, D1, A1.
study_report:
    move.w  sr, -(sp)
    ori.w   #$0700, sr
    move.w  UFW_SW_WRAPS, d1
    moveq   #0, d0
    move.b  MFP_TADR.w, d0
    neg.b   d0                            ; ticks into this wrap
    btst    #5, MFP_IPRA.w                ; a wrap not counted yet?
    beq.s   .sr_ok
    tst.b   d0
    bmi.s   .sr_ok                        ; it came after the read
    addq.w  #1, d1
.sr_ok:
    lsl.w   #8, d1
    or.w    d1, d0
    bra.s   study_out
study_send:
    move.w  sr, -(sp)
    ori.w   #$0700, sr
study_out:
    lea     STUDY_POINT_WINDOW, a1
    tst.b   (a1, d2.w)
    move.w  d0, d1
    lsr.w   #8, d1
    lea     STUDY_HI_WINDOW, a1
    tst.b   (a1, d1.w)
    and.w   #$FF, d0
    lea     STUDY_LO_WINDOW, a1
    tst.b   (a1, d0.w)
    move.w  (sp)+, sr
    rts
    endc

; -------------------------------------------------------------------
; userfw_reset_stub -- copied to UFW_RESET_STUB and run there by
; .cold_reset: position-independent, and it reads nothing from the
; cartridge. Waits UFW_PRE_RESET_WAIT (a couple of seconds on an 8 MHz
; ST), clears TOS's memory-valid magics so it takes this for a power-on,
; and jumps through the reset vector.
userfw_reset_stub:
    move.l  #UFW_PRE_RESET_WAIT, d0
.wait:
    subq.l  #1, d0
    bne.s   .wait
    clr.l   $420.w                    ; memvalid
    clr.l   $43A.w                    ; memval2
    clr.l   $51A.w                    ; memval3
    movea.l $4.w, a0
    jmp     (a0)
userfw_reset_stub_end:

; The Microwire commands userfw sends on the DMA path (see MW_DATA):
; %10 then a 3-bit command and its value.
userfw_mw_cmds:
    dc.w    $04E8                         ; master volume 0 dB
    dc.w    $0554                         ; left 0 dB
    dc.w    $0514                         ; right 0 dB
    dc.w    $0486                         ; treble flat
    dc.w    $0446                         ; bass flat
    dc.w    $0401                         ; mix: the YM with the DMA sound
userfw_mw_cmds_end:

; The NOP tail. This module is the last in the cartridge image, and
; firmware.py strips trailing zero bytes from it: the last word must be
; harmless to drop or to prefetch past. An app that includes
; inc/sidecart_functions.s here must keep the tail after it, so the
; senders' wait loop is never the image's last code (see main.s).
    even
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
userfw_end:
