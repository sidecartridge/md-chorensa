#!/usr/bin/env python3
"""The translated game as images for the RP's flash store (xl_store.h), from
the user's copy of the program, for one firmware build (a store build:
XLAT_STORE=1, rp/src/CMakeLists.txt): COMMON.IMG, what more than one scene
uses, and one image a scene for the slot: STAGE1.IMG..STAGE7.IMG and
MENUS.IMG (the title, CONFIG, KAKUSI). STAGE1.IMG also carries the logic
bench's snapshot (rp/src/bench_logic.c). They go on the SD card
(tools/dev/swd.py sd-put), and the firmware copies them into the store.

1. The whole game on the host, the scenes as phases (XL_PHASES: the stages'
   routines and the menus'), with the trace and the memory map: a played
   game through both loops (invincible), one to its game over and name
   entry, the CONFIG and KAKUSI menus, and the attract demo (reused with
   --runs and --game-over). Per phase they record the functions entered,
   the pages written and read, and the patterns drawn.
2. A snapshot in the first stage (the attract demo's) and runs resumed from
   it, for the bench: the functions entered, the memory touched (the demo's
   and a scripted player's), the patterns drawn to the stage's end.
3. Each function's home: a scene's when only that scene runs it, COMMON's
   when more than one does, or the game over and the name entry (which come
   in any stage), and for the snapshot's resume entries (the bench's outer
   frames, which a scene change must not take away); none when only the boot
   does (the RP starts from images).
4. Each scene's pages: the ones it writes, or reads after an earlier scene
   wrote them, in RAM (in every stage too the object pool and the game
   over's), each page in one slot of the pool for every scene (a scene
   change moves none; a page whose slot another scene's page took gets its
   content after the boot back on entry: scenes.txt models it on the host,
   xl_host.c's XL_SCENES); the ones it only reads in its image, or in COMMON's when another
   scene reads them too; the text plane in its layout; the graphics pages,
   the X68000's sprite patterns and, in the stages, its BG maps a sink (the
   RP draws the art, and nothing reads them back). The contents from the
   snapshot.
5. Each scene's art: the background in the scene's palette (--palettes:
   palette_fit.py's stageN.bin and menus.bin), its own patterns; the
   patterns more than one scene draws in COMMON.
6. The translations (xlat.py --image), compiled for the RP and linked at
   their regions' addresses: COMMON against the firmware's symbols, each
   scene against the firmware's and COMMON's; their size and CRC, and the
   files; beside them EFFECTS.SE8, the effects the host decoded (the
   snapshot run's effects.bin), which the RP plays from the card.

Everything it writes is the game's: outside the repository (default
~/chorensa/xlat-store), never committed.

usage: rp_store.py --firmware ELF [--frame 650] [--runs RUN ... --game-over RUN]
                   [--palettes DIR] [--out DIR]
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
import rp_art  # noqa: E402
import rp_image  # noqa: E402
import swd  # noqa: E402
import xlat  # noqa: E402

# The first stage's frames in the attract demo (its routine from 645, the
# second stage's from 10,380): its patterns gathered to there.
STAGE1_LAST = 10380
# The bench plays from the snapshot to here (rp/src/bench_logic.c's
# LAST_FRAME): into the second stage, through a scene change.
BENCH_LAST = 12000
# The scripted player of the host (XL_LIVE_FROM: flying, shooting, a bomb
# every six seconds) takes over the demo here; its memory map runs to before
# it loses its last life.
LIVE_FROM, LIVE_UNTIL = 1000, 2900

# The phases (XL_PHASES, numbered from 1 in this order; 0 the boot): the
# stages' routines (the dispatcher's targets: STAGE_0, the seven stages,
# STAGE_7_PLUS), then MAIN_MENU, CONFIG_MODE_MENU and KAKUSI_MODE_MENU.
PHASE_ENTRIES = (0x2BA68, 0x2BBE4, 0x2C52E, 0x2D15C, 0x2DD78, 0x2F080, 0x2FBA0, 0x30D72, 0x320D2,
                 0x24D5A, 0x251B6, 0x25780)
SCENE_OF_PHASE = {**{n + 1: f"stage{n}" for n in range(1, 8)}, 10: "menus", 11: "menus",
                  12: "menus"}
SCENES = tuple(f"stage{n}" for n in range(1, 8)) + ("menus",)
SCENE_KIND = {**{f"stage{n}": n for n in range(1, 8)}, "menus": 8}  # xl_store.h's kinds
# The memory after the boot (the title's frame BOOT_FRAME, every file
# loaded): what a scene change sets a page to when its slot changed hands.
BOOT_FRAME = 100
# The whole game's runs (step 1): name, frames, input script, environment.
RUNS = (("whole-live", 210000, "whole-game.txt", {"XL_INVINCIBLE": "1"}),
        ("whole-gameover", 30000, "whole-game.txt", {}),
        ("whole-config", 2500, "menu-config.txt", {}),
        ("whole-kakusi", 6000, "menu-kakusi.txt", {}),
        ("whole-attract", 150000, None, {}))
GAME_OVER_RUN = "whole-gameover"
INPUTS = os.path.join(HERE, "inputs")
PAGE = rp_image.PAGE
# The game's object pool (LONG_0009C8E4, its table, and L_0009CEE4, 48 KB):
# a stage uses any of its slots, so every stage keeps all of it in RAM.
OBJECT_POOL = (0x9C8E4, 0xA8EE4)
# The X68000's sprite patterns and BG maps: the port shows neither; nothing
# reads the patterns back, and only the menus the maps (through IOCS $CE).
SPRITE_PATTERNS = (0xEB8000, 0xEBC000)
BG_MAPS = (0xEBC000, 0xEC0000)
# The graphics pages (GVRAM): the menus draw the background into them when a
# game starts, and nothing reads them back (the RP draws the scene's art).
GVRAM = (0xC00000, 0xE00000)

HEADER_SIZE = 176         # sizeof(xl_store_header_t)
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


def pages_of(lo, hi):
    return set(range(lo // PAGE, (hi + PAGE - 1) // PAGE))


def byte_map_pages(path):
    """A memory map's (memmap.bin) pages: (written, read)."""
    data = open(path, "rb").read()
    written, read = set(), set()
    for p in range(len(data) // PAGE):
        flags = 0
        for b in data[p * PAGE:(p + 1) * PAGE]:
            flags |= b
        if flags & 6:
            written.add(p)
        elif flags & 1:
            read.add(p)
    return written, read


class Scenes:
    """What each scene runs, writes, reads and draws over the whole game's
    runs; the game over and name entry as their own (`gameover`): what the
    game-over run's stage did that no other run's same stage did."""

    def __init__(self, runs, game_over):
        self.entries = {s: set() for s in SCENES + ("gameover",)}
        self.ram = {s: set() for s in SCENES + ("gameover",)}
        self.read = {s: set() for s in SCENES + ("gameover",)}
        self.patterns = {s: set() for s in SCENES + ("gameover",)}
        self.boot = set()
        for run in runs:
            if run != game_over:
                self._add(run, self.entries, self.ram, self.read, self.patterns)
        e, w, r, p = ({s: set() for s in SCENES} for _ in range(4))
        self._add(game_over, e, w, r, p)
        for s in SCENES:
            if s == "menus":
                for d, x in ((self.entries, e), (self.ram, w), (self.read, r), (self.patterns, p)):
                    d[s] |= x[s]
                continue
            self.entries["gameover"] |= e[s] - self.entries[s]
            self.ram["gameover"] |= w[s] - self.ram[s]
            self.read["gameover"] |= r[s] - self.read[s]
            self.patterns["gameover"] |= p[s] - self.patterns[s]

    def _add(self, run, entries, ram, read, patterns):
        for e, bits in coverage(os.path.join(run, "coverage.txt")).items():
            for ph in range(32):
                if bits >> ph & 1:
                    if ph in SCENE_OF_PHASE:
                        entries[SCENE_OF_PHASE[ph]].add(e)
                    elif ph == 0:
                        self.boot.add(e)
        phases = [int(line.split()[0]) for line in open(os.path.join(run, "memmap_phases.txt"))
                  if not line.startswith("#")]
        since = set()  # written by the phases of play before
        for k, ph in enumerate(phases):
            m = open(os.path.join(run, f"memmap_{k:02d}.bin"), "rb").read()
            written = {p for p in range(len(m)) if m[p] & 1}
            scene = SCENE_OF_PHASE.get(ph)
            if scene:
                ram[scene] |= written | {p for p in range(len(m)) if m[p] & 2 and p in since}
                read[scene] |= {p for p in range(len(m)) if m[p] & 2 and p not in since}
            if ph != 0:
                since |= written
        for path in glob.glob(os.path.join(run, "pattern_use_p*.bin")):
            scene = SCENE_OF_PHASE.get(int(path[-6:-4]))
            if scene:
                patterns[scene] |= rp_art.read_pattern_use(path)

    def homes(self, exe, listing_path, resume, bench):
        """Each entry of the resumed translation: its scene's when only one
        scene runs it, 'common' when more do (or the game over, or the bench's
        stage beside another), none when only the boot does."""
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
            if e not in plain.entries:
                # A resume entry (the rest of a function the snapshot was in):
                # the bench's outer frames, which last through scene changes.
                homes[e] = "common"
                continue
            # An entry the runs never entered (a resumed translation's, inside
            # a function) goes with the function it is in.
            seen = e in self.boot or e in bench or any(e in x for x in self.entries.values())
            key = e if seen else owner.get(e)
            scenes = {s for s in SCENES if key in self.entries[s]}
            if key in bench:
                scenes.add("stage1")
            if key in self.entries["gameover"] or len(scenes) > 1:
                homes[e] = "common"
            elif scenes:
                homes[e] = scenes.pop()
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
  .text : {{ KEEP(*(.xl_header)) KEEP(*(.rodata.xl_common_*)) KEEP(*(.text.xl_stub_*))
            *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(4); }} > IMAGE
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


def header_c(path, image, kind, firmware_id, entries, scene=False, bench=None, layout=None,
             songs=None, scene_entries=None):
    """The image's header (xl_store.h's xl_store_header_t)."""
    fields = ["XL_STORE_MAGIC", "XL_STORE_VERSION", f"{kind}u", "0", "0", f'"{firmware_id}"']
    named = {"entries": f"xl_{image}_entries", "entry_count": f"{entries}u",
             "song_playing": "0xFFFFFFFFu"}
    if scene:
        named.update({"ram_list": "xl_rp_ram_list", "ram_count": "XL_RP_RAM_COUNT",
                      "regions": "xl_stage_regions", "art": "&xl_stage_art",
                      "entry_pages": "XL_RP_ENTRY_COUNT", "entry_init": "xl_rp_entry_init"})
    if bench:
        named.update({"resume": f"0x{bench['resume']:06X}u", "state": "xl_rp_state",
                      "sprites": "xl_rp_sprites", "ram_init": "xl_rp_ram_init",
                      "checkpoints": "xl_rp_checkpoints", "checkpoint_count": "XL_RP_CHECKPOINTS",
                      "song_playing": f"{bench['song']}u", "song_frames": f"{bench['song_frames']}u",
                      "text_rows": "xl_rp_text_rows", "text_init": "xl_rp_text_init",
                      "text_row_count": "XL_RP_TEXT_ROWS"})
    if layout:
        named["layout"] = "{" + ", ".join(f"0x{layout[n]:06X}u" for n in LAYOUT) + "}"
    if songs:
        named.update({"songs": "k_songs", "song_count": f"{len(songs)}u"})
    if scene_entries:
        named.update({"scenes": "k_scenes", "scene_count": f"{len(scene_entries)}u"})
    with open(path, "w") as f:
        f.write("/* Generated by tools/xlat/rp_store.py: never commit. */\n")
        f.write('#include "xl_store.h"\n')
        if scene:
            f.write('#include "xl_rp_image.h"\n')
            f.write("extern const xl_store_art_t xl_stage_art;\n")
        f.write(f"extern const xl_store_entry_t xl_{image}_entries[];\n")
        if songs:
            f.write("static const xl_store_song_t k_songs[] = {\n" + "".join(
                f'  {{0x{a:06X}u, "{n}"}},\n' for a, n in songs) + "};\n")
        if scene_entries:
            f.write("static const xl_store_scene_entry_t k_scenes[] = {\n" + "".join(
                f"  {{0x{a:06X}u, {k}u}},\n" for a, k in scene_entries) + "};\n")
        f.write(f"__attribute__((section(\".xl_header\"), used)) const xl_store_header_t "
                f"xl_{image}_header = {{\n")
        for name, value in zip(("magic", "version", "kind", "size", "crc", "firmware"), fields):
            f.write(f"  .{name} = {value},\n")
        for name, value in named.items():
            f.write(f"  .{name} = {value},\n")
        f.write("};\n")


def check_firmware_fits(elf):
    """The firmware ends before the store starts."""
    end = swd.elf_symbols(elf, "__flash_binary_end").get("__flash_binary_end")
    if end is None:
        sys.exit(f"{elf} has no __flash_binary_end")
    if end[0] > store_layout()["XL_STORE_BASE"]:
        sys.exit(f"{elf} ends at 0x{end[0]:08X}, past the store's start "
                 f"0x{store_layout()['XL_STORE_BASE']:08X}")


def firmware_id(elf):
    """The firmware's build ID (its release_build_id), as the header keeps it."""
    build_id = swd.elf_build_id(elf)
    if not build_id:
        sys.exit(f"{elf} has no release_build_id")
    return build_id[:31]


def whole_game_runs():
    """Step 1's runs, made on the host: {name: folder}."""
    env = {"XL_TRACE": "1", "XL_MEMMAP": "1",
           "XL_PHASES": ",".join(f"{a:06X}" for a in PHASE_ENTRIES)}
    runs = {}
    for name, frames, script, extra in RUNS:
        more = {"XL_INPUT": os.path.join(INPUTS, script)} if script else {}
        runs[name] = host_run(name, ["--frames", str(frames)], {**env, **more, **extra},
                              check=False)
    return runs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--firmware", required=True, help="the store build's rp.elf")
    ap.add_argument("--frame", type=int, default=650, help="the bench's snapshot's frame")
    ap.add_argument("--runs", nargs="+", help="the whole game's runs (else they are made)")
    ap.add_argument("--game-over", help="which of --runs plays to a game over")
    ap.add_argument("--palettes", default=os.path.join(os.path.expanduser("~"), "chorensa",
                                                       "xlat-store", "palettes"),
                    help="the scenes' palette fits: stage1.bin..stage7.bin, menus.bin")
    ap.add_argument("--out", default=os.path.join(os.path.expanduser("~"), "chorensa", "xlat-store"))
    args = ap.parse_args()
    if bool(args.runs) != bool(args.game_over):
        sys.exit("--runs and --game-over go together")
    cfg = recfg.load()
    original = recfg.need(cfg, "ORIGINAL")[0]
    exe = cfg.get("SZ2X") or os.path.join(os.path.expanduser("~"), "chorensa", "SZ2.X")
    listing = os.path.join(original, "sources", "sz2.s")
    fw = os.path.abspath(args.firmware)
    fw_id = firmware_id(fw)
    check_firmware_fits(fw)
    palettes = {s: os.path.join(args.palettes, f"{s}.bin") for s in SCENES}
    missing = [p for p in palettes.values() if not os.path.exists(p)]
    if missing:
        sys.exit("no palette fit: " + " ".join(missing))

    if args.runs:
        runs, game_over = args.runs, args.game_over
    else:
        made = whole_game_runs()
        runs, game_over = list(made.values()), made[GAME_OVER_RUN]
    snap = host_run(f"store-snap{args.frame}", ["--snapshot", str(args.frame)])
    bench = host_run("store-coverage", ["--resume", snap], {"XL_TRACE": "1"})
    memmap = host_run("store-memmap", ["--resume", snap], {"XL_MEMMAP": "1"})
    live = host_run("store-live-memmap", ["--resume", snap, "--frames", str(LIVE_UNTIL)],
                    {"XL_MEMMAP": "1", "XL_LIVE_FROM": str(LIVE_FROM)}, check=False)
    stage_run = host_run("store-stage1", ["--resume", snap, "--frames", str(STAGE1_LAST)])
    checks = host_run("store-checkpoints", ["--resume", snap, "--frames", str(BENCH_LAST)])
    boot_run = host_run("store-boot", ["--frames", str(BOOT_FRAME + 1)],
                        {"XL_MEMORY_AT": str(BOOT_FRAME)})
    boot_memory = open(os.path.join(boot_run, "memory.bin"), "rb").read()
    resume = os.path.join(snap, "resume.txt")
    print("runs " + " ".join(runs) + f"\nsnapshot {snap}")

    scenes = Scenes(runs, game_over)
    homes = scenes.homes(exe, listing, resume, set(coverage(os.path.join(bench, "coverage.txt"))))
    os.makedirs(args.out, exist_ok=True)
    homes_file = os.path.join(args.out, "homes.txt")
    with open(homes_file, "w") as f:
        f.write("# entry home (rp_store.py)\n")
        for e in sorted(homes):
            f.write(f"{e:06X} {homes[e]}\n")
    count = {h: sum(1 for v in homes.values() if v == h) for h in ("common",) + SCENES}
    print("homes: " + ", ".join(f"{count[h]} {h}" for h in ("common",) + SCENES))

    work = os.path.join(args.out, "build")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    jobs = [subprocess.Popen([sys.executable, os.path.join(HERE, "xlat.py"), "--executable", exe,
                              "--listing", listing, "--out", os.path.join(work, image),
                              "--resume", resume, "--homes", homes_file, "--image", image],
                             stdout=subprocess.DEVNULL)
            for image in ("common",) + SCENES]
    if any(j.wait() for j in jobs):
        sys.exit("a translation failed")
    layout = {m.group(1): int(m.group(2), 16) for m in re.finditer(
        r"#define XL_(\w+) 0x([0-9A-F]+)u", open(os.path.join(work, "common", "xl_layout.h")).read())}

    # The pages (step 4).
    state, sprites, memory = rp_image.read_snapshot(os.path.join(snap, "snapshot.bin"))
    runtime_written, runtime_read = rp_image.runtime_pages(layout)
    text = set(rp_image.text_pages())
    gvram = pages_of(*GVRAM)
    sink = pages_of(*SPRITE_PATTERNS)
    bench_written, bench_read = set(), set()
    for run in (memmap, live):
        w, r = byte_map_pages(os.path.join(run, "memmap.bin"))
        bench_written |= w
        bench_read |= r
    ram, read = {}, {}
    for s in SCENES:
        ram[s] = scenes.ram[s] | runtime_written
        read[s] = scenes.read[s] | runtime_read
        if s != "menus":
            ram[s] |= scenes.ram["gameover"] | pages_of(*OBJECT_POOL)
            read[s] |= scenes.read["gameover"]
            ram[s] -= pages_of(*BG_MAPS)
            read[s] -= pages_of(*BG_MAPS)
        ram[s] -= sink
        read[s] -= sink
        if s == "stage1":
            ram[s] |= bench_written
            read[s] |= bench_read
        ram[s] -= text | gvram
        read[s] -= text | gvram | ram[s]
    slot, shared = rp_image.assign_slots(ram)
    print(f"RAM: {max(slot.values()) + 1} slots of the pool's {rp_image.SPARE} for "
          f"{len(slot)} pages, {len(shared)} sharing theirs")
    # The host's model of the scene changes (xl_host.c's XL_SCENES).
    with open(os.path.join(args.out, "scenes.txt"), "w") as f:
        f.write("# rp_store.py: the scenes' RAM pages and their slots (xl_host.c's XL_SCENES)\n")
        f.write(f"memory {os.path.join(boot_run, 'memory.bin')}\n")
        for ph, scene in sorted(SCENE_OF_PHASE.items()):
            f.write(f"phase {ph} {SCENES.index(scene)}\n")
        for k, scene in enumerate(SCENES):
            for p in sorted(ram[scene]):
                f.write(f"page {k} {p} {slot[p]} {int(p in shared)}\n")
    readers = {}
    for s in SCENES:
        for p in read[s]:
            readers.setdefault(p, set()).add(s)
    common_pages = sorted(p for p, r in readers.items() if len(r) > 1)
    common_at = {p: k * PAGE for k, p in enumerate(common_pages)}
    data = rp_image.swapped(b"".join(memory[p * PAGE:(p + 1) * PAGE] for p in common_pages))
    with open(os.path.join(work, "common", "xl_common_data.c"), "w") as f:
        f.write("/* Generated by tools/xlat/rp_store.py from the user's copy of the program:\n"
                " * never commit. */\n#include <stdint.h>\n\n")
        f.write(f"const uint8_t xl_common_pages[{max(len(data), 1)}] __attribute__((aligned(4))) = "
                f"{{\n{rp_image.c_bytes(data) or '  0,'}\n}};\n")
    print(f"COMMON's read-only pages: {len(common_pages)} ({len(data) // 1024} KB)")

    # The patterns (step 5).
    drawn = {s: set(scenes.patterns[s]) for s in SCENES}
    for s in SCENES:
        if s != "menus":
            drawn[s] |= scenes.patterns["gameover"]
    drawn["stage1"] |= rp_art.read_pattern_use(os.path.join(stage_run, "pattern_use.bin"))
    users = {}
    for s in SCENES:
        for p in drawn[s]:
            users.setdefault(p, set()).add(s)
    common_patterns = sorted(p for p, u in users.items() if len(u) > 1)
    common_index = {p: k for k, p in enumerate(common_patterns)}
    size = rp_art.write_common_patterns(os.path.join(work, "common", "xl_common_art.c"), memory,
                                        layout, common_patterns)
    print(f"COMMON's patterns: {len(common_patterns)} ({size // 1024} KB)")

    resume_at = struct.unpack("<2I", open(os.path.join(snap, "snapshot.bin"), "rb").read(8))[1]
    songs, playing = read_songs(snap)
    song_names = [n for _a, n in songs]
    common_h = os.path.join(work, "common_header.c")
    scene_entries = sorted((e, SCENE_KIND[h]) for e, h in homes.items() if h in SCENE_KIND)
    header_c(common_h, "common", 0, fw_id, count["common"], layout=layout, songs=songs,
             scene_entries=scene_entries)
    common_elf, common_bin = build_image("common", os.path.join(work, "common"), "XL_STORE_COMMON",
                                         fw, [], work, common_h)
    print(f"common: {len(common_bin) // 1024} KB", flush=True)
    images = [("COMMON.IMG", common_bin)]
    for s in SCENES:
        out = os.path.join(work, s)
        bench_state = None
        if s == "stage1":
            data = open(os.path.join(checks, "checkpoints.bin"), "rb").read()
            n = struct.unpack(">I", data[4:8])[0]
            bench_state = {"state": state, "sprites": sprites, "checkpoints": data[8:8 + 32 * n]}
        figures = rp_image.write_image(out, memory, {p: slot[p] for p in ram[s]},
                                       read[s] - set(common_at),
                                       {p: common_at[p] for p in read[s] & set(common_at)},
                                       boot_memory, shared, bench_state)
        own = drawn[s] - set(common_index)
        background, pattern_size = rp_art.write_art(
            os.path.join(out, "xl_stage_art.c"), memory, layout, rp_art.read_palette(palettes[s]),
            own, {p: common_index[p] for p in drawn[s] & set(common_index)})
        # The scene links against COMMON's symbols but its own: COMMON's stubs
        # carry the scene's functions' names.
        stripped = os.path.join(work, f"common-symbols-{s}.elf")
        names = os.path.join(work, f"{s}-own.txt")
        with open(names, "w") as f:
            f.write("".join(f"f_{e:06X}\n" for e, h in homes.items() if h == s))
        subprocess.run([tool("objcopy"), f"--strip-symbols={names}", common_elf, stripped],
                       check=True)
        header = os.path.join(work, f"{s}_header.c")
        bench_header = None
        if s == "stage1":
            song = song_names.index(playing[0]) if playing and playing[0] in song_names else 0xFFFFFFFF
            bench_header = {"resume": resume_at, "song": song,
                            "song_frames": args.frame - playing[1] if playing else 0}
        header_c(header, s, SCENE_KIND[s], fw_id, count[s], scene=True, bench=bench_header)
        _elf, binary = build_image(s, out, "XL_STORE_SLOT", fw, [stripped], work, header)
        print(f"{s}: {len(binary) // 1024} KB: {figures}; background {background // 1024} KB, "
              f"{len(own)} own patterns ({pattern_size // 1024} KB)", flush=True)
        images.append((f"{s.upper()}.IMG", binary))

    for name, data in images:
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes, CRC {struct.unpack_from('<I', data, CRC_OFFSET)[0]:08X}, "
              f"for {fw_id}")
    shutil.copyfile(os.path.join(snap, "effects.bin"), os.path.join(args.out, "EFFECTS.SE8"))
    print(f"EFFECTS.SE8: {os.path.getsize(os.path.join(args.out, 'EFFECTS.SE8'))} bytes")


if __name__ == "__main__":
    main()
