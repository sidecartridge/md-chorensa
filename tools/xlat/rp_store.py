#!/usr/bin/env python3
"""The translated game as images for the RP's flash store (xl_store.h), from
the user's copy of the program, for one firmware build (a store build:
XLAT_STORE=1, rp/src/CMakeLists.txt): COMMON.IMG, the code every stage
shares, and STAGE1.IMG, the first stage's own code with the logic bench's
snapshot (rp/src/bench_logic.c). They go on the SD card (tools/dev/swd.py
sd-put), and the firmware copies them into the store.

1. The attract mode with the stages as phases (run_host.py, XL_PHASES):
   which functions each stage runs (reused with --phases RUN).
2. A snapshot after the stage load and two runs resumed from it: one with
   the trace (the functions it enters), one with the memory map (and another
   with a scripted player taking over: what live input writes); a third to
   the end of the first stage (the patterns its sprites draw), and the
   attract demo to there with each pixel's palette entry (the palette fit).
3. Each function's home: the first stage's if only that stage runs it, the
   common image's if anything else does; none if nothing does.
4. The two translations (xlat.py --image), compiled for the RP and linked at
   their regions' addresses: the common image against the firmware's
   symbols, the stage's against the firmware's and the common image's.
5. The first stage's art (palette_fit.py, rp_art.py) in its image.
6. The headers' size and CRC, and the files.

Everything it writes is the game's: outside the repository (default
~/chorensa/xlat-store), never committed.

usage: rp_store.py --firmware ELF [--frame 650] [--phases RUN] [--entries RUN] [--out DIR]
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "re"))
sys.path.insert(0, os.path.join(HERE, "..", "dev"))
import program  # noqa: E402
import recfg  # noqa: E402
import swd  # noqa: E402
import xlat  # noqa: E402

# The first stage's frames in the attract demo (its routine from 645, the
# second stage's from 10,380): the palette is fitted and the patterns
# gathered over them.
STAGE1_LAST = 10380
# The scripted player of the host (XL_LIVE_FROM: flying, shooting, a bomb
# every six seconds) takes over the demo here; its memory map runs to before
# it loses its last life (a game over writes the text plane and much else).
LIVE_FROM, LIVE_UNTIL = 1000, 2900

# The stages' routines (the dispatcher's targets), in the order XL_PHASES
# numbers them from 1: phase 2 is the first stage.
STAGES = (0x2BA68, 0x2BBE4, 0x2C52E, 0x2D15C, 0x2DD78, 0x2F080, 0x2FBA0, 0x30D72, 0x320D2)
FIRST_STAGE_PHASE = 2
PHASE_FRAMES = 150000
HEADER_SIZE = 144         # sizeof(xl_store_header_t)
LAYOUT = ("SPRITE_DATA_TABLE", "CURRENT_SPRITE_DATA_ENTRY", "VBL_VWAIT_COUNTER", "PLAYER_SCORE",
          "WORD_00088E6C", "BACKGROUND_SCROLL_COUNTER", "PLAYER_INFO_STRUCT",
          "SIN_COS_TABLES_START")  # xl_store.h's XL_LAYOUT_* order
SIZE_OFFSET, CRC_OFFSET = 12, 16

CFLAGS = ["-mcpu=cortex-m0plus", "-mthumb", "-Os", "-std=c11", "-w", "-ffunction-sections",
          "-fdata-sections", "-fno-common", "-DXL_PAGED"]


def host_run(name, extra, env=None, check=True):
    """run_host.py with these arguments, with the music driver (XL_MUSIC):
    the run's folder (it must PASS, unless not `check`: a run the player
    changed cannot match the oracle)."""
    out = subprocess.run([sys.executable, os.path.join(HERE, "run_host.py"), "--frames", "6000",
                          "--screens", "0", "--name", name] + extra, capture_output=True,
                         text=True, env={**os.environ, "XL_MUSIC": "1", **(env or {})})
    lines = out.stdout.splitlines()
    run = next((line for line in lines if os.path.isdir(line)), None)
    if out.returncode not in (0, 1) or run is None or (check and "PASS" not in lines):
        sys.exit(f"run_host.py {name} failed:\n{out.stdout[-2000:]}{out.stderr[-2000:]}")
    return run


def tool(name):
    base = os.environ.get("PICO_TOOLCHAIN_PATH")
    path = os.path.join(base, f"arm-none-eabi-{name}") if base else shutil.which(f"arm-none-eabi-{name}")
    if not path or not os.path.exists(path):
        sys.exit(f"no arm-none-eabi-{name}: set PICO_TOOLCHAIN_PATH")
    return path


def store_layout():
    """xl_store.h's addresses and sizes: hex values, or the name of another."""
    text = open(os.path.join(HERE, "xl_store.h")).read()
    values = {}
    for name, value in re.findall(r"#define (XL_STORE_\w+) (0x[0-9A-Fa-f]+u|XL_STORE_\w+)", text):
        values[name] = values[value] if value in values else int(value.rstrip("u"), 16)
    return values


def coverage(path):
    """{entry: phase bits} from a coverage.txt."""
    out = {}
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.split()
        out[int(p[0], 16)] = int(p[4], 16) if len(p) > 4 else 0
    return out


def homes_for(exe, listing_path, resume, phases, bench):
    """Each entry of the resumed translation: 'stageN' when only stage N runs
    it, 'common' when anything else does, none when nothing does."""
    image = program.Image(exe)
    listing = program.Listing(listing_path, bss=image.bss)
    plain = xlat.Program(image, listing)
    owner = {}
    for e in sorted(plain.entries):
        for a in plain.function(e):
            owner.setdefault(a, e)
    resumed = xlat.Program(image, listing, resume)
    homes = {}
    for e in sorted(resumed.entries):
        bits = phases.get(e, phases.get(owner.get(e), 0))
        stage = bits.bit_length() - FIRST_STAGE_PHASE  # bit FIRST_STAGE_PHASE: stage 1
        if bits and bits & (bits - 1) == 0 and stage >= 1:
            homes[e] = f"stage{stage}"
        elif bits or e in bench:
            homes[e] = "common"
    return homes


def build_image(name, src_dir, layout_key, firmware, extra_symbols, out_dir, header_c):
    """Compile src_dir's C and header_c, link at the region; the .elf and .bin."""
    origin = store_layout()[layout_key]
    size = store_layout()[layout_key + "_SIZE"]
    objs = []
    jobs = []
    sources = sorted(glob.glob(os.path.join(src_dir, "xl_*.c"))) + [header_c]
    for c in sources:
        o = os.path.join(out_dir, f"{name}-{os.path.basename(c)[:-2]}.o")
        objs.append(o)
        jobs.append(subprocess.Popen([tool("gcc"), *CFLAGS, "-I", HERE, "-I", src_dir, "-c", c,
                                      "-o", o]))
    if any(j.wait() for j in jobs):
        sys.exit(f"{name}: the build failed")
    script = os.path.join(out_dir, f"{name}.ld")
    with open(script, "w") as f:
        f.write(f"""MEMORY {{ IMAGE (rx) : ORIGIN = 0x{origin:08X}, LENGTH = 0x{size:X} }}
SECTIONS {{
  .text : {{ KEEP(*(.xl_header)) *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(4); }} > IMAGE
  .data : {{ *(.data .data.*) }} > IMAGE
  .bss : {{ *(.bss .bss.* COMMON) }} > IMAGE
  /DISCARD/ : {{ *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes) }}
}}
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "a store image has no writable data")
""")
    elf = os.path.join(out_dir, f"{name}.elf")
    symbols = [f"-Wl,--just-symbols={s}" for s in [firmware, *extra_symbols]]
    # The divisions through the firmware's wrappers (the SDK's, on the RP's
    # hardware divider); libgcc for the rest (the switch tables' helpers).
    divisions = [f"-Wl,--defsym=__aeabi_{d}=__wrap___aeabi_{d}"
                 for d in ("idiv", "idivmod", "uidiv", "uidivmod", "lmul", "ldivmod", "uldivmod")]
    subprocess.run([tool("gcc"), "-mcpu=cortex-m0plus", "-mthumb", "-nostdlib", "-T", script,
                    *objs, *symbols, *divisions, "-lgcc", "-Wl,--gc-sections", "-Wl,--entry=0",
                    "-o", elf], check=True)
    binary = os.path.join(out_dir, f"{name}.bin")
    subprocess.run([tool("objcopy"), "-O", "binary", "-j", ".text", elf, binary], check=True)
    data = bytearray(open(binary, "rb").read())
    struct.pack_into("<I", data, SIZE_OFFSET, len(data))
    struct.pack_into("<I", data, CRC_OFFSET, zlib.crc32(bytes(data[HEADER_SIZE:])) & 0xFFFFFFFF)
    if len(data) > size:
        sys.exit(f"{name}: {len(data)} bytes, the region holds {size}")
    return elf, bytes(data)


def read_songs(snap):
    """The songs the game had loaded at the snapshot, and the one playing:
    ([(address, name)], (index, frames played) or None)."""
    songs, playing = [], None
    for line in open(os.path.join(snap, "songs.txt")):
        p = line.split()
        if p and p[0] == "load":
            songs.append((int(p[1], 16), os.path.splitext(os.path.basename(p[2].replace("\\", "/")))[0].upper()))
        elif p and p[0] == "playing":
            name = os.path.splitext(os.path.basename(p[1]))[0].upper()
            playing = (name, int(p[2]))
    return songs, playing


def header_c(path, image, kind, firmware_id, entries, stage=None, layout=None, songs=None):
    with open(path, "w") as f:
        f.write("/* Generated by tools/xlat/rp_store.py: never commit. */\n")
        f.write('#include "xl_store.h"\n')
        if stage:
            f.write('#include "xl_rp_image.h"\n')
            f.write("extern const xl_store_art_t xl_stage_art;\n")
        f.write(f"extern const xl_store_entry_t xl_{image}_entries[];\n")
        if songs:
            f.write("static const xl_store_song_t k_songs[] = {\n" + "".join(
                f'  {{0x{a:06X}u, "{n}"}},\n' for a, n in songs) + "};\n")
        f.write(f"__attribute__((section(\".xl_header\"), used)) const xl_store_header_t "
                f"xl_{image}_header = {{\n")
        f.write(f"  XL_STORE_MAGIC, XL_STORE_VERSION, {kind}u, 0, 0, \"{firmware_id}\",\n")
        f.write(f"  xl_{image}_entries, {entries}u,\n")
        if stage:
            f.write(f"  0x{stage['resume']:06X}u, xl_rp_state, xl_rp_sprites, xl_rp_ram_init,\n"
                    f"  XL_RP_WRITTEN_PAGES, xl_stage_regions, xl_rp_checkpoints, XL_RP_CHECKPOINTS,\n")
        else:
            f.write("  0, 0, 0, 0, 0, 0, 0, 0,\n")
        if layout:
            f.write("  {" + ", ".join(f"0x{layout[n]:06X}u" for n in LAYOUT) + "},\n")
        else:
            f.write("  {0},\n")
        f.write("  &xl_stage_art,\n" if stage else "  0,\n")
        if songs:
            f.write(f"  k_songs, {len(songs)}u, 0xFFFFFFFFu, 0,\n")
        elif stage:
            f.write(f"  0, 0, {stage['song']}u, {stage['song_frames']}u,\n")
        f.write("};\n")


def firmware_id(elf):
    """The firmware's build ID (its release_build_id), as the header keeps it."""
    build_id = swd.elf_build_id(elf)
    if not build_id:
        sys.exit(f"{elf} has no release_build_id")
    return build_id[:31]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--firmware", required=True, help="the store build's rp.elf")
    ap.add_argument("--frame", type=int, default=650, help="the snapshot's frame")
    ap.add_argument("--phases", help="a run with the stages as phases (else one is made)")
    ap.add_argument("--entries", help="a run with the first stage's entries (else one is made)")
    ap.add_argument("--out", default=os.path.join(os.path.expanduser("~"), "chorensa", "xlat-store"))
    args = ap.parse_args()
    cfg = recfg.load()
    original = recfg.need(cfg, "ORIGINAL")[0]
    exe = cfg.get("SZ2X") or os.path.join(os.path.expanduser("~"), "chorensa", "SZ2.X")
    listing = os.path.join(original, "sources", "sz2.s")
    fw = os.path.abspath(args.firmware)
    fw_id = firmware_id(fw)

    phases_run = args.phases or host_run(
        "store-phases", ["--frames", str(PHASE_FRAMES)],
        {"XL_TRACE": "1", "XL_PHASES": ",".join(f"{a:06X}" for a in STAGES)})
    snap = host_run(f"store-snap{args.frame}", ["--snapshot", str(args.frame)])
    bench = host_run("store-coverage", ["--resume", snap], {"XL_TRACE": "1"})
    memmap = host_run("store-memmap", ["--resume", snap], {"XL_MEMMAP": "1"})
    # And a scripted player flying from frame LIVE_FROM until before its game
    # over: the memory live input writes beside the demo's.
    live = host_run("store-live-memmap", ["--resume", snap, "--frames", str(LIVE_UNTIL)],
                    {"XL_MEMMAP": "1", "XL_LIVE_FROM": str(LIVE_FROM)}, check=False)
    resume = os.path.join(snap, "resume.txt")
    entries_run = args.entries or host_run(
        "store-entries", ["--frames", str(STAGE1_LAST), "--screens", "10", "--entries"])
    stage_run = host_run("store-stage1", ["--resume", snap, "--frames", str(STAGE1_LAST)])
    print(f"phases {phases_run}\nsnapshot {snap}\nentries {entries_run}")

    homes = homes_for(exe, listing, resume, coverage(os.path.join(phases_run, "coverage.txt")),
                      set(coverage(os.path.join(bench, "coverage.txt"))))
    os.makedirs(args.out, exist_ok=True)
    homes_file = os.path.join(args.out, "homes.txt")
    with open(homes_file, "w") as f:
        f.write("# entry home (rp_store.py)\n")
        for e in sorted(homes):
            f.write(f"{e:06X} {homes[e]}\n")
    count = {h: sum(1 for v in homes.values() if v == h) for h in set(homes.values())}
    print("homes: " + ", ".join(f"{count[h]} {h}" for h in sorted(count)))

    work = os.path.join(args.out, "build")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    for image in ("common", "stage1"):
        subprocess.run([sys.executable, os.path.join(HERE, "xlat.py"), "--executable", exe,
                        "--listing", listing, "--out", os.path.join(work, image),
                        "--resume", resume, "--homes", homes_file, "--image", image],
                       check=True, stdout=subprocess.DEVNULL)
    stage_dir = os.path.join(work, "stage1")
    subprocess.run([sys.executable, os.path.join(HERE, "rp_image.py"),
                    "--snapshot", os.path.join(snap, "snapshot.bin"),
                    "--memmap", os.path.join(memmap, "memmap.bin"),
                    "--memmap", os.path.join(live, "memmap.bin"),
                    "--checkpoints", os.path.join(bench, "checkpoints.bin"),
                    "--layout", os.path.join(stage_dir, "xl_layout.h"),
                    "--out", stage_dir], check=True)
    resume_at = struct.unpack("<2I", open(os.path.join(snap, "snapshot.bin"), "rb").read(8))[1]
    palette = os.path.join(work, "palette1.bin")
    subprocess.run([sys.executable, os.path.join(HERE, "palette_fit.py"), entries_run,
                    "--first", str(args.frame), "--last", str(STAGE1_LAST), "--out", palette],
                   check=True)
    subprocess.run([sys.executable, os.path.join(HERE, "rp_art.py"),
                    "--snapshot", os.path.join(snap, "snapshot.bin"), "--palette", palette,
                    "--patterns", os.path.join(stage_run, "pattern_use.bin"),
                    "--layout", os.path.join(stage_dir, "xl_layout.h"), "--out", stage_dir],
                   check=True)

    common_h = os.path.join(work, "common_header.c")
    names = {m.group(1): int(m.group(2), 16) for m in re.finditer(
        r"#define XL_(\w+) 0x([0-9A-F]+)u", open(os.path.join(work, "common", "xl_layout.h")).read())}
    songs, playing = read_songs(snap)
    song_names = [n for _a, n in songs]
    header_c(common_h, "common", 0, fw_id, count["common"], layout=names, songs=songs)
    common_elf, common_bin = build_image("common", os.path.join(work, "common"), "XL_STORE_COMMON",
                                         fw, [], work, common_h)
    # The stage links against the common image's symbols but its own: the
    # common image's stubs carry the stage functions' names.
    stripped = os.path.join(work, "common-symbols.elf")
    own = os.path.join(work, "stage-own.txt")
    with open(own, "w") as f:
        f.write("".join(f"f_{e:06X}\n" for e, h in homes.items() if h == "stage1"))
    subprocess.run([tool("objcopy"), f"--strip-symbols={own}", common_elf, stripped], check=True)
    stage_h = os.path.join(work, "stage_header.c")
    song = song_names.index(playing[0]) if playing and playing[0] in song_names else 0xFFFFFFFF
    header_c(stage_h, "stage1", 1, fw_id, count["stage1"],
             {"resume": resume_at, "song": song,
              "song_frames": args.frame - playing[1] if playing else 0})
    _stage_elf, stage_bin = build_image("stage1", stage_dir, "XL_STORE_STAGE", fw, [stripped],
                                        work, stage_h)

    for name, data in (("COMMON.IMG", common_bin), ("STAGE1.IMG", stage_bin)):
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes, CRC {struct.unpack_from('<I', data, CRC_OFFSET)[0]:08X}, "
              f"for {fw_id}")


if __name__ == "__main__":
    main()
