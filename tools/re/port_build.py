#!/usr/bin/env python3
"""Builds the Amiga port of the original from the user's copy of its
repository (ORIGINAL in the harness's config), into a folder outside this
repository, for amiga_run.py --program.

The port's own build (its build.sh, the Amiga lines) on a copy of its
sources, with two optional changes:

  --invincible        the boot sets the hidden menu's invincibility instead of
                      clearing it (`clr.w INVINCIBILITY_MODE` at $26AF8 becomes
                      `addq.w #1,INVINCIBILITY_MODE`, the same size: no address
                      moves), so the port replays a game recorded invincible
                      (the host's XL_INVINCIBLE) as it was played
  --measured-frames N the port's harness measures N frames, not 65,536
                      (`MAXIMUM_MEASURED_FRAMES`, its emulator.s): a recorded
                      game runs longer than the attract demo
  --lockstep          the port's lockstep build (-D__LOCKSTEP__ for its
                      emulator and parallax): the VBL's work at a fixed point
                      of the game's frame and a screen every 100 frames, as
                      the host build draws them, so screens compare pixel for
                      pixel
  --scripted          the port's input as the host's when a script drives it
                      (amiga_run.py --input, run_host.py's XL_INPUT): no auto
                      fire (the original has none; the port presses a held
                      trigger A again every 2 frames in a stage), stick 2
                      reads nothing pressed, and when the game reads the
                      stick more than 64 times in one game frame (its loops
                      that wait for the buttons with no frame wait) the
                      buttons read released, as the host's script does
  --dump-sprites A-B  the port's harness keeps each sprite list the game hands
                      XSP_OUT in frames A to B (its frame_count: frames
                      completed) and writes them to sprites.bin at the end,
                      as the host's XL_SPRITE_DUMP does: the entries (u32),
                      then each frame (u32), sprites (u32) and 512 slots of
                      8 bytes, big endian; up to 16 lists

Without any, the program is the one the port ships, byte for byte.

usage: port_build.py --out DIR [--invincible] [--measured-frames N] [--lockstep]
                     [--scripted] [--dump-sprites A-B]
"""

import argparse
import os
import shutil
import subprocess
import sys

import recfg

INVINCIBLE_FROM = "\tclr.w   INVINCIBILITY_MODE                           ; 00026AF8 4279 0008 DB0C\n"
INVINCIBLE_TO = "\taddq.w  #$1,INVINCIBILITY_MODE                      ; 00026AF8 (port_build.py --invincible)\n"
MEASURED_FROM = "MAXIMUM_MEASURED_FRAMES=65536\n"
MODULES = (("main", "68000"), ("mem_map", "68030"), ("emulator", "68030"), ("graphics", "68030"),
           ("input", "68030"), ("display", "68030"), ("audio", "68030"), ("parallax", "68030"))
LINK = ("amiga_main", "sz2_amiga", "amiga_mem_map", "amiga_emulator", "amiga_input", "amiga_display",
        "amiga_audio", "amiga_parallax", "amiga_graphics")


# --scripted: (file, the text, what replaces it).
SCRIPTED = (
    ("input.s", "\ttst.b\tamiga_stage_frame\n\tbeq\t\t.no_autofire\n",
     "\tbra\t\t.no_autofire ; port_build.py --scripted: no auto fire\n"),
    ("input.s", ".done:\n\tmove.l\t(sp)+,d1\n\n\trts\n\n; Returns d0.b = joystick directions",
     """.done:
	; port_build.py --scripted: more than 64 reads in one game frame (a
	; loop with no frame wait), the buttons read released.
	move.l	frame_count,d1
	cmp.l	scripted_read_frame,d1
	beq		.same_frame
	move.l	d1,scripted_read_frame
	clr.l	scripted_reads
	bra		.counted
.same_frame:
	addq.l	#1,scripted_reads
.counted:
	cmp.l	#64,scripted_reads
	bls		.not_spinning
	or.b	#$60,d0
.not_spinning:
	move.l	(sp)+,d1

	rts

scripted_read_frame:
	dc.l	-1
scripted_reads:
	dc.l	0

; Returns d0.b = joystick directions"""),
    ("emulator.s", "\tjsr\t\tjoystick_now ; The current state: the game polls it in loops.\n",
     "\tmoveq\t#-1,d0 ; port_build.py --scripted: stick 2 reads nothing pressed\n"
     "\ttst.b\td1\n\tbne\t\tpoll_render_check\n"
     "\tjsr\t\tjoystick_now ; The current state: the game polls it in loops.\n"),
)
# --dump-sprites: (file, the text it follows, what is inserted after it).
DUMP_SPRITES = (
    ("graphics.s", "\txref SPRITE_DATA_TABLE\n", "\txref dump_sprite_list\n"),
    ("graphics.s", "prepare_sprite_infos:\n", "\tjsr\t\tdump_sprite_list\n"),
    ("emulator.s", "\txref frame_sprite_count\n",
     "\txref SPRITE_DATA_TABLE\n\txref CURRENT_SPRITE_DATA_ENTRY\n\txdef dump_sprite_list\n"
     "SPRITE_DUMP_ENTRIES=16\nSPRITE_DUMP_ENTRY=8+512*8\n"),
    ("emulator.s", "\tjsr\t\twrite_graphics_dump\n", "\tjsr\t\twrite_sprite_dump\n"),
    ("emulator.s", "\tdc.b\t'graphics.bin',0\n", "sprite_dump_file_name:\n\tdc.b\t'sprites.bin',0\n\teven\n"),
    ("emulator.s", "frame_count:\n\tds.l\t1\n",
     "sprite_dump_count:\n\tds.l\t1\nsprite_dump:\n\tds.b\tSPRITE_DUMP_ENTRIES*SPRITE_DUMP_ENTRY\n"),
)
# The routines, before write_graphics_dump (text): the list copied at each
# XSP_OUT of the chosen frames, and written at the end.
DUMP_ROUTINES = """dump_sprite_list:
	movem.l	d0-d1/a0-a1,-(sp)
	move.l	frame_count,d0
	sub.l	#DUMP_SPRITES_FROM,d0
	cmp.l	#DUMP_SPRITES_FRAMES,d0
	bcc		.done
	move.l	sprite_dump_count,d0
	cmp.l	#SPRITE_DUMP_ENTRIES,d0
	bcc		.done
	addq.l	#1,sprite_dump_count
	mulu.l	#SPRITE_DUMP_ENTRY,d0
	lea		sprite_dump,a1
	add.l	d0,a1
	move.l	frame_count,(a1)+
	move.l	CURRENT_SPRITE_DATA_ENTRY,d0
	sub.l	#SPRITE_DATA_TABLE,d0
	lsr.l	#3,d0
	cmp.l	#512,d0
	bls		.count_ok
	move.l	#512,d0
.count_ok:
	move.l	d0,(a1)+
	lea		SPRITE_DATA_TABLE,a0
	subq.l	#1,d0
	bmi		.done
.copy:
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	dbf		d0,.copy
.done:
	movem.l	(sp)+,d0-d1/a0-a1
	rts

write_sprite_dump:
	move.l	dos_base,a6
	move.l	#sprite_dump_file_name,d1
	move.l	#MODE_NEWFILE,d2
	jsr		_LVOOpen(a6)
	move.l	d0,d4
	beq		.done
	move.l	d4,d1
	move.l	#sprite_dump_count,d2
	moveq	#4,d3
	jsr		_LVOWrite(a6)
	move.l	d4,d1
	move.l	#sprite_dump,d2
	move.l	sprite_dump_count,d3
	mulu.l	#SPRITE_DUMP_ENTRY,d3
	jsr		_LVOWrite(a6)
	move.l	d4,d1
	jsr		_LVOClose(a6)
.done:
	rts

"""


def replace_once(path, old, new):
    text = open(path, encoding="latin-1").read()
    if text.count(old) != 1:
        sys.exit(f"{path}: the line to change is not there once (another version of the port?)")
    with open(path, "w", encoding="latin-1") as f:
        f.write(text.replace(old, new))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--invincible", action="store_true")
    ap.add_argument("--measured-frames", type=int)
    ap.add_argument("--lockstep", action="store_true")
    ap.add_argument("--scripted", action="store_true")
    ap.add_argument("--dump-sprites", metavar="A-B")
    args = ap.parse_args()
    original, = recfg.need(recfg.load(), "ORIGINAL")
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    for d in ("sources", "tools"):
        shutil.copytree(os.path.join(original, d), os.path.join(out, d))
    os.makedirs(os.path.join(out, "binaries", "amiga"))
    if args.invincible:
        replace_once(os.path.join(out, "sources", "sz2.s"), INVINCIBLE_FROM, INVINCIBLE_TO)
    if args.measured_frames:
        replace_once(os.path.join(out, "sources", "amiga", "emulator.s"), MEASURED_FROM,
                     f"MAXIMUM_MEASURED_FRAMES={args.measured_frames} ; port_build.py\n")
    if args.scripted:
        for name, old, new in SCRIPTED:
            replace_once(os.path.join(out, "sources", "amiga", name), old, new)
    defines = []
    if args.dump_sprites:
        first, last = (int(v) for v in args.dump_sprites.split("-"))
        amiga = os.path.join(out, "sources", "amiga")
        for name, after, insert in DUMP_SPRITES:
            replace_once(os.path.join(amiga, name), after, after + insert)
        replace_once(os.path.join(amiga, "emulator.s"), "write_graphics_dump:\n",
                     DUMP_ROUTINES + "write_graphics_dump:\n")
        defines = [f"-DDUMP_SPRITES_FROM={first}", f"-DDUMP_SPRITES_FRAMES={last - first + 1}"]
    suffix = "_mac" if sys.platform == "darwin" else ""
    vasm = os.path.join(out, "tools", "vasmm68k_mot" + suffix)
    vlink = os.path.join(out, "tools", "vlink" + suffix)

    def run(cmd):
        subprocess.run(cmd, cwd=out, check=True)

    for name, cpu in MODULES:
        lockstep = ["-D__LOCKSTEP__"] if args.lockstep and name in ("emulator", "parallax") else []
        run([vasm, f"sources/amiga/{name}.s", "-quiet", "-Fhunk", f"-m{cpu}"] + lockstep + defines +
            ["-o", f"binaries/amiga_{name}.o"])
    run([vasm, "sources/sz2.s", "-quiet", "-no-opt", "-Fhunk", "-m68030", "-D__AMIGA__", "-o",
         "binaries/sz2_amiga.o"])
    run([vlink] + [f"binaries/{m}.o" for m in LINK] + ["-bamigahunk", "-s", "-o", "binaries/amiga/sz2"])
    program = os.path.join(out, "binaries", "amiga", "sz2")
    shipped = os.path.join(original, "binaries", "amiga", "sz2")
    same = open(program, "rb").read() == open(shipped, "rb").read()
    print(f"{program}: {os.path.getsize(program)} bytes, "
          + ("the shipped program" if same else "differs from the shipped program"))


if __name__ == "__main__":
    main()
