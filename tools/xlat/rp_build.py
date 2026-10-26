#!/usr/bin/env python3
"""The translated game for the RP's logic bench (rp/src/bench_logic.c), from
the user's copy of the program, in one folder the RP's build takes
(XLAT_RP_DIR):

1. the attract demo on the host to a snapshot at frame F (run_host.py);
2. two runs resumed from it: one with the trace (the functions it enters),
   one with the memory map (the bytes it reads and writes), both checked
   against the oracle;
3. the translation with the snapshot's resume entries, only those functions
   in full (the others report themselves if reached);
4. the memory in pages (rp_image.py).

The folder is the game's: outside the repository (default
~/chorensa/xlat-rp), never committed.

usage: rp_build.py [--frame 650] [--out DIR]
then:  XLAT_RP_DIR=DIR tools/dev/flash.sh release
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "re"))
import recfg  # noqa: E402


def host_run(name, extra, env=None):
    """run_host.py with these arguments: the run's folder (it must PASS)."""
    out = subprocess.run([sys.executable, os.path.join(HERE, "run_host.py"), "--frames", "6000",
                          "--screens", "0", "--name", name] + extra,
                         capture_output=True, text=True, env={**os.environ, **(env or {})})
    lines = out.stdout.splitlines()
    run = next((line for line in lines if os.path.isdir(line)), None)
    if out.returncode != 0 or run is None or "PASS" not in lines:
        sys.exit(f"run_host.py {name} failed:\n{out.stdout[-2000:]}{out.stderr[-2000:]}")
    return run


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--frame", type=int, default=650, help="the snapshot's frame")
    ap.add_argument("--out", default=os.path.join(os.path.expanduser("~"), "chorensa", "xlat-rp"))
    args = ap.parse_args()
    cfg = recfg.load()
    original = recfg.need(cfg, "ORIGINAL")[0]
    exe = cfg.get("SZ2X") or os.path.join(os.path.expanduser("~"), "chorensa", "SZ2.X")

    snap = host_run(f"rp-snap{args.frame}", ["--snapshot", str(args.frame)])
    print(f"snapshot: {snap}")
    trace = host_run("rp-coverage", ["--resume", snap], {"XL_TRACE": "1"})
    memmap = host_run("rp-memmap", ["--resume", snap], {"XL_MEMMAP": "1"})
    print(f"resumed: {trace}, {memmap}")

    os.makedirs(args.out, exist_ok=True)
    for f in os.listdir(args.out):
        if f.startswith("xl_") and f.endswith((".c", ".h", ".bin")):
            os.remove(os.path.join(args.out, f))
    subprocess.run([sys.executable, os.path.join(HERE, "xlat.py"), "--executable", exe,
                    "--listing", os.path.join(original, "sources", "sz2.s"), "--out", args.out,
                    "--resume", os.path.join(snap, "resume.txt"),
                    "--only", os.path.join(trace, "coverage.txt")], check=True)
    os.remove(os.path.join(args.out, "xl_image.bin"))  # the host's; the pages hold the memory
    subprocess.run([sys.executable, os.path.join(HERE, "rp_image.py"),
                    "--snapshot", os.path.join(snap, "snapshot.bin"),
                    "--memmap", os.path.join(memmap, "memmap.bin"),
                    "--checkpoints", os.path.join(trace, "checkpoints.bin"),
                    "--layout", os.path.join(args.out, "xl_layout.h"),
                    "--out", args.out], check=True)
    print(f"XLAT_RP_DIR={args.out}")


if __name__ == "__main__":
    main()
