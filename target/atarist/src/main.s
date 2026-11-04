; Firmware loader from cartridge
; (C) 2023-2025 by Diego Parrilla
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.
; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .	 
; bit 30: TOS application .
; bit 31: TTP

; The cartridge window's layout, the command values and the ROM3 signalling
; windows, shared with userfw.s (must match rp/src/include/cart_shared.h).
	include inc/sidecart_layout.s

FBDRV_ADDR		equ (ROM4_ADDR + $2000)				; $FA2000 (MOVEM loop cart->ST screen copy)

; Left over from the removed mono boot UI, which filled the first 8000
; bytes of the framebuffer with a 1bpp u8g2 image that the
; .print_loop_low copy loop expanded to the 32000-byte ST screen. The
; native 4bpp copy replaced it; nothing references this constant.
MONO_UI_BUFFER_SIZE	equ 8000


; If 1, the display will not use the framebuffer and will write directly to the
; display memory. This is useful to reduce the memory usage in the rp2040
; When not using the framebuffer, the endianness swap must be done in the atari ST
DISPLAY_BYPASS_FRAMEBUFFER 	equ 1

_conterm			equ $484	; Conterm device number
PALETTE_REGS		equ $FFFF8240	; the 16 palette words


; Constants needed for the commands
RANDOM_TOKEN_POST_WAIT:   equ $1                             ; Wait cycles after the RNG is ready
COMMAND_TIMEOUT           equ $0000FFFF                      ; Timeout for the command
COMMAND_WRITE_TIMEOUT     equ COMMAND_TIMEOUT                ; Timeout for write commands

CMD_MAGIC_NUMBER    	  equ ($ABCD) 					  ; Magic number header to identify a command
CMD_RETRIES_COUNT	  	  equ 3							  ; Number of retries for the command
CMD_SET_SHARED_VAR		  equ 1							  ; This is a fake command to set the shared variables
														  ; Used to store the system settings
; App commands for the terminal
APP_TERMINAL 				equ $0 ; The terminal app

; App terminal commands
APP_TERMINAL_START   		equ $0 ; Start terminal command
APP_TERMINAL_KEYSTROKE 		equ $1 ; Keystroke command

_dskbufp                equ $4c6                            ; Address of the disk buffer pointer
; _p_cookies ($5a0) lives in inc/sidecart_functions.s (also used by
; that file's detect_hw).


	include inc/sidecart_macros.s
	include inc/tos.s



; Macros
; XBIOS Vsync wait
vsync_wait          macro
					move.w #37,-(sp)
					trap #14
					addq.l #2,sp
                    endm    

; XBIOS GetRez
; Return the current screen resolution in D0
get_rez				macro
					move.w #4,-(sp)
					trap #14
					addq.l #2,sp
					endm

; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

; Check the left or right shift key. If pressed, exit.
check_shift_keys	macro
					move.w #-1, -(sp)			; Read all key status
					move.w #$b, -(sp)			; BIOS Get shift key status
					trap #13
					addq.l #4,sp

					btst #1,d0					; Left shift skip and boot GEM
					bne boot_gem

					btst #0,d0					; Right shift skip and boot GEM
					bne boot_gem

					endm

; Check the keys pressed
check_keys			macro

					gemdos	Cconis,2		; Check if a key is pressed
					tst.l d0
					beq .\@no_key

					gemdos	Cnecin,2		; Read the key pressed

					cmp.b #27, d0		; Check if the key is ESC
					beq .\@esc_key	; If it is, send terminal command

					move.l d0, d3
					send_sync APP_TERMINAL_KEYSTROKE, 4

					bra .\@no_key
.\@esc_key:
					send_sync APP_TERMINAL_START, 0

.\@no_key:

					endm

check_commands		macro
					move.l CMD_MAGIC_SENTINEL_ADDR, d6	; Store in the D6 register the remote command value
					cmp.l #CMD_TERMINAL, d6		; Check if the command is a terminal command
					bne.s .\@check_reset

					; Check the keys for the terminal emulation
					check_keys
					bra .\@bypass
.\@check_reset:
					cmp.l #CMD_RESET, d6		; Check if the command is a reset
					beq .reset					; If it is, reset the computer
					cmp.l #CMD_BOOT_GEM, d6		; Check if the command is to boot GEM
					beq boot_gem				; If it is, boot GEM
					cmp.l #CMD_START, d6		; Check if the command hands over to USERFW
					beq rom_function			; If it is, jump to the user firmware dispatcher

					; If we are here, the command is a NOP
					; If the command is a NOP, check the shift keys to bypass the command
					; check_shift_keys
					check_keys
.\@bypass:
					endm

	section

;Rom cartridge
; The cartridge image (header + code below) MUST fit in
; CARTRIDGE_CODE_SIZE = $4000 (16 KB). The hard limit is enforced by
; target/atarist/build.sh after vlink emits BOOT.BIN; any direct vasm /
; vlink invocation that bypasses the build script is unchecked, so keep
; an eye on BOOT.BIN's size when iterating outside ./build.sh.

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
;	dc.l second
	dc.l 0
	dc.l GAME_TOS_APP				; no init routine: a TOS application
	dc.l game_tos					; opened from the desktop's drive c:
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_game_tos - game_tos
	dc.b "GAME.TOS",0
    even

; GAME.TOS: the desktop starts it as any TOS program (Pexec, user mode, its
; basepage at 4(sp)); nothing of the cartridge runs at boot. The program
; enters supervisor mode, checks the machine and the RP's word, and runs
; userfw, which takes the machine over and returns (rts) when the player
; leaves to the desktop; then the program gives the desktop back its
; resolution, palette and user mode, and ends (Pterm0).
;
; userfw keeps its screens, the DMA sound ring and its state at fixed
; addresses, GAME_RAM_LOW..GAME_RAM_HIGH (userfw.s: UFW_SCREEN_A ..
; UFW_RESET_STUB's state area). A program started by the desktop owns its
; whole TPA and keeps it (no Mshrink), so those addresses are the program's
; as long as the TPA holds them; the stack TOS gives the program sits at the
; TPA's top, where they may be, so the program runs on its own stack at the
; TPA's bottom instead. The words it keeps for itself are in its basepage's
; command line, which a cartridge program does not read.
GAME_TOS_APP		equ $40000000	; CA_INIT bit 30: a TOS application
GAME_RAM_LOW		equ $00060000	; userfw's lowest fixed address (UFW_SCREEN_A)
GAME_RAM_HIGH		equ $00078000	; past its highest (the state area's end)
GAME_STACK_BYTES	equ 4096
BP_HITPA			equ 4			; basepage: the TPA's end
BP_CMDLIN			equ $80			; basepage: the command line, 128 bytes
GAME_SAVE_SSP		equ BP_CMDLIN		; long: Super's old stack
GAME_SAVE_SP		equ BP_CMDLIN+4		; long: the stack TOS gave the program
GAME_SAVE_REZ		equ BP_CMDLIN+8		; word: the desktop's resolution
GAME_SAVE_PALETTE	equ BP_CMDLIN+16	; 16 words: the desktop's palette

game_tos:
	move.l 4(sp), a4				; the basepage
	clr.l -(sp)
	gemdos Super,6
	move.l d0, GAME_SAVE_SSP(a4)
	move.l sp, GAME_SAVE_SP(a4)

	; userfw's fixed addresses in this program's TPA, its own stack below them.
	lea .no_memory_txt(pc), a0
	cmpi.l #GAME_RAM_HIGH, BP_HITPA(a4)
	blo .refuse
	lea 256+GAME_STACK_BYTES(a4), a1
	cmpa.l #GAME_RAM_LOW, a1
	bhi .refuse
	move.l a1, sp
	move.l a4, -(sp)

	; Low resolution only; a medium-resolution desktop is switched to low
	; and back. High resolution (a mono monitor) cannot show the game.
	get_rez
	move.w d0, GAME_SAVE_REZ(a4)
	lea .highres_unsupported_txt(pc), a0
	cmpi.w #2, d0
	beq .refuse_on_own_stack
	; The RP's word: 0 starts the game, else its reason is printed.
	lea BOOT_MESSAGE_ADDR, a0
	tst.w BOOT_STATUS_ADDR
	bne .refuse_on_own_stack

	movem.l PALETTE_REGS.w, d0-d7
	movem.l d0-d7, GAME_SAVE_PALETTE(a4)
	tst.w GAME_SAVE_REZ(a4)
	beq.s .low_rez
	clr.w -(sp)						; low resolution
	move.l #-1, -(sp)				; the screens as they are
	move.l #-1, -(sp)
	move.w #5, -(sp)				; XBIOS Setscreen
	trap #14
	lea 12(sp), sp
.low_rez:

	jsr USERFW

	move.l (sp)+, a4
	move.w GAME_SAVE_REZ(a4), d0
	beq.s .rez_back
	move.w d0, -(sp)
	move.l #-1, -(sp)
	move.l #-1, -(sp)
	move.w #5, -(sp)				; XBIOS Setscreen
	trap #14
	lea 12(sp), sp
.rez_back:
	movem.l GAME_SAVE_PALETTE(a4), d0-d7
	movem.l d0-d7, PALETTE_REGS.w
	bra.s .leave

.refuse_on_own_stack:
	move.l (sp)+, a4
.refuse:
	move.l a0, -(sp)
	gemdos Cconws,6
	print .crlf_txt
	print .press_key_txt
	gemdos Cnecin,2

.leave:
	move.l GAME_SAVE_SP(a4), sp
	move.l GAME_SAVE_SSP(a4), -(sp)
	gemdos Super,6
	Pterm0

.crlf_txt:
	dc.b $d,$a,0
.press_key_txt:
	dc.b "Press a key.",$d,$a,0
.no_memory_txt:
	dc.b "Not enough free memory for the game.",$d,$a
	dc.b "Remove accessories or a RAM disk and try again.",0
.highres_unsupported_txt:
	dc.b "High resolution (640x400) not supported.",$d,$a
	dc.b "Switch to low or medium resolution.",0
	even

; Dispatcher for the user firmware module. Reached on CMD_START via the
; sentinel poll in check_commands. The cartridge image places userfw.s
; at offset $0800 (USERFW = $FA0800) through target/atarist/src/userfw.ld;
; main.s simply hands control over with a one-way jmp. Apps that want
; to chain multiple modules can change this to a sequence of jsr / jmp
; the same way md-drives-emulator's rom_function dispatches into
; GEMDRIVE/FLOPPYEMUL/ACSIEMUL/RTCEMUL.
rom_function:
    jmp USERFW

; Shared functions included at the end of the file
; Don't forget to include the macros for the shared functions at the top of file
    include "inc/sidecart_functions.s"

; The NOP tail. The senders' wait loop must never be the last code of a module:
; firmware.py strips trailing zero bytes from the image, and both the write sender
; (its code size includes 4 bytes past the loop) and the 68000's prefetch read
; past the loop's last word. Every module that includes sidecart_functions.s
; ends like this.
	even
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
main_end:

end_game_tos:
	even
	dc.l 0