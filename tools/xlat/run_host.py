#!/usr/bin/env python3
"""Translate the user's copy of the program, build it for the host with its
runtime (host/xl_host.c), and play the attract demo against the oracle.

One folder per run under RUNS (tools/re/re.cfg), as the other harnesses:
command.txt, the data the program ran with (copies of the port's *_DAT
folders and sincos.dat: the program writes into them), checkpoints.bin,
screen_NNNNN.bin every --screens frames (the port's dump format), output.txt,
result.txt (PASS when every checkpoint matches the port's reference, a shorter
run against its start, and, with --screen-reference, every screen matches the
port's of the same frame in RGB). The translation and the build
go to BUILD (default ~/chorensa/xlat-build), outside the repository: they
are the game's.

usage: run_host.py [--frames 6000] [--name NAME] [--build DIR] [--rebuild]
                   [--screens N] [--screen-reference DIR] [--snapshot F] [--resume RUN]

--snapshot F writes snapshot.bin and resume.txt into the run at frame F;
--resume RUN plays on from that run's snapshot, translated with its resume
entries (a build folder of its own).
"""

import argparse
import datetime
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "re"))
import oracle  # noqa: E402
import recfg   # noqa: E402

# XL_TRACE=1 / XL_MEMMAP=1 in the environment: the host's trace / memory-map
# build, in a build folder of its own.
VARIANTS = [v for v in ("XL_TRACE", "XL_MEMMAP") if os.environ.get(v)]
CFLAGS = ["-std=c11", "-O1", "-w"] + ["-D" + v for v in VARIANTS]
RP_SRC = os.path.join(HERE, "..", "..", "rp", "src")


def build(cfg, out, rebuild, resume=None, art=None):
    original = cfg["ORIGINAL"]
    exe = cfg.get("SZ2X") or os.path.join(os.path.expanduser("~"), "chorensa", "SZ2.X")
    listing = os.path.join(original, "sources", "sz2.s")
    tag = os.path.join(out, ".translated")
    sources = [os.path.join(HERE, f) for f in ("xlat.py", "m68k.py", "program.py", "xl.h")]
    sources += [resume] if resume else []
    stale = rebuild or not os.path.exists(tag) or any(
        os.path.getmtime(s) > os.path.getmtime(tag) for s in sources)
    if stale:
        shutil.rmtree(out, ignore_errors=True)
        subprocess.run([sys.executable, os.path.join(HERE, "xlat.py"), "--executable", exe,
                        "--listing", listing, "--out", out]
                       + (["--resume", resume] if resume else []), check=True)
        open(tag, "w").close()
    objects = []
    jobs = []
    sources = sorted(glob.glob(os.path.join(out, "xl_*.c"))) + [os.path.join(HERE, "host", "xl_host.c")]
    flags = list(CFLAGS)
    if art:
        # The RP's renderer and the stage's art beside the host's renderer.
        sources += [os.path.join(RP_SRC, "xl_render.c"), os.path.join(art, "xl_stage_art.c")]
        flags += ["-DXL_RP_RENDER", "-I", os.path.join(RP_SRC, "include")]
    for c in sources:
        o = os.path.join(out, os.path.basename(c)[:-2] + ".o")
        objects.append(o)
        if stale or not os.path.exists(o) or os.path.getmtime(c) > os.path.getmtime(o):
            jobs.append(subprocess.Popen(["cc", *flags, "-I", HERE, "-I", out, "-c", c, "-o", o]))
    if any(j.wait() for j in jobs):
        sys.exit("the build failed")
    binary = os.path.join(out, "xl_host")
    subprocess.run(["cc", *objects, "-o", binary], check=True)
    return binary


def rgb(path):
    width, lines, _, palette, pixels = oracle.read_screen(path)
    return width, lines, b"".join(oracle.rgb_rows(width, lines, palette, pixels))


def compare_screens(run, reference):
    """The run's screens against the reference's of the same frames, in RGB;
    PNGs of both and of the differing pixels for the first few that differ."""
    ours = sorted(glob.glob(os.path.join(run, "screen_*.bin")))
    same, differ = 0, []
    for path in ours:
        theirs = os.path.join(reference, os.path.basename(path))
        if not os.path.exists(theirs):
            continue
        width, height, a = rgb(path)
        w2, h2, b = rgb(theirs)
        if (width, height) != (w2, h2):
            differ.append((os.path.basename(path), width * height))
            continue
        pixels = sum(1 for k in range(0, len(a), 3) if a[k:k + 3] != b[k:k + 3])
        if pixels == 0:
            same += 1
            continue
        differ.append((os.path.basename(path), pixels))
        if len(differ) <= 5:
            stem = os.path.join(run, os.path.basename(path)[:-4])
            rows = lambda d: [d[y * 3 * width:(y + 1) * 3 * width] for y in range(height)]
            oracle.write_png(stem + "-ours.png", width, height, rows(a))
            oracle.write_png(stem + "-port.png", width, height, rows(b))
            mask = bytearray(a)
            for k in range(0, len(a), 3):
                if a[k:k + 3] != b[k:k + 3]:
                    mask[k:k + 3] = b"\xff\x00\xff"
            oracle.write_png(stem + "-diff.png", width, height, rows(bytes(mask)))
    lines = [f"screens compared {same + len(differ)} of {len(ours)}, {same} identical"]
    for name, pixels in differ[:10]:
        lines.append(f"screen {name}: {pixels} pixels differ")
    if same + len(differ) == 0:
        lines.append("screens none compared")
    elif not differ:
        lines.append("screens MATCH")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--frames", type=int, default=6000)
    ap.add_argument("--name", default="")
    ap.add_argument("--build", default=os.path.join(os.path.expanduser("~"), "chorensa", "xlat-build"))
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--screens", type=int, default=500, help="a screen every N frames")
    ap.add_argument("--screen-reference", help="a folder of the port's screen_NNNNN.bin")
    ap.add_argument("--snapshot", type=int, help="write a snapshot at this frame")
    ap.add_argument("--resume", metavar="RUN", help="start from the snapshot of an earlier run")
    ap.add_argument("--rp-art", metavar="DIR", help="with the RP's renderer and this stage art "
                    "(rp_art.py's xl_stage_art.c): rpscreen_NNNNN.bin beside each screen")
    ap.add_argument("--entries", action="store_true",
                    help="beside each screen, each pixel's palette entry (entries_NNNNN.bin)")
    args = ap.parse_args()
    cfg = recfg.load()
    original, runs = recfg.need(cfg, "ORIGINAL", "RUNS")
    build_dir = args.build + "".join("-" + v[3:].lower() for v in VARIANTS)
    resume = os.path.abspath(args.resume) if args.resume else None
    if resume:
        build_dir += "-resume"
    if args.rp_art:
        build_dir += "-render"
    binary = build(cfg, build_dir, args.rebuild, resume and os.path.join(resume, "resume.txt"),
                   args.rp_art and os.path.abspath(args.rp_art))

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run = os.path.join(runs, f"{stamp}-xlat" + (f"-{args.name}" if args.name else ""))
    data = os.path.join(run, "data")
    os.makedirs(data)
    game = os.path.join(original, "binaries", "amiga")
    for d in sorted(glob.glob(os.path.join(game, "*_DAT"))):
        if os.path.basename(d) != "MUSIC_DAT":
            shutil.copytree(d, os.path.join(data, os.path.basename(d)))
    shutil.copy(os.path.join(original, "sources", "sincos.dat"), data)
    shutil.copy(os.path.join(build_dir, "xl_image.bin"), run)
    command = [binary, data, str(args.frames), run, "--screens", str(args.screens)]
    if args.snapshot:
        command += ["--snapshot", str(args.snapshot)]
    if resume:
        command += ["--resume", os.path.join(resume, "snapshot.bin")]
    if args.entries:
        command += ["--entries", "1"]
    with open(os.path.join(run, "command.txt"), "w") as f:
        f.write(" ".join(command) + "\n")
    with open(os.path.join(run, "output.txt"), "w") as out:
        proc = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT)
    lines = [f"exit {proc.returncode}"]
    checkpoints = os.path.join(run, "checkpoints.bin")
    verdict = 1
    if os.path.exists(checkpoints):
        rows = oracle.read_checkpoints(checkpoints)
        lines.append(f"checkpoints {len(rows)}")
        reference = os.path.join(original, "tests", "amiga", "reference", "attract-6000.txt")
        difference = oracle.compare(oracle.read_checkpoints(reference), rows, prefix=True)
        if difference:
            lines.append("compare frame {} field {}: {} expected, {}".format(*difference))
        elif rows:
            lines.append(f"compare MATCH {os.path.basename(reference)}")
            verdict = 0
    if args.screen_reference:
        lines += compare_screens(run, args.screen_reference)
        if not lines[-1].startswith("screens MATCH"):
            verdict = 1
    lines.insert(0, "PASS" if proc.returncode == 0 and verdict == 0 else "FAIL")
    with open(os.path.join(run, "result.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(run)
    print("\n".join(lines))
    print(open(os.path.join(run, "output.txt")).read()[-1500:])
    return 0 if lines[0] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
