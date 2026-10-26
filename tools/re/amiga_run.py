#!/usr/bin/env python3
"""Runs the Amiga port of the original in FS-UAE, headless, one folder per run.

The port runs the original X68000 program unchanged on a 68020/68030. Given a
frame count it plays the attract demo for that many frames and writes its
checkpoints (checkpoints.bin) and a screen dump every 500 frames
(screen_NNNNN.bin): our reference for the original (see oracle.py).

Each run is a folder <RUNS>/<date>-amiga-<config>[-name]/ with command.txt,
fs-uae.log, output.txt (the program's own text), WORK/ (the drive the program
ran from: its copy of the program and the data), the results, and result.txt.

--program runs another build of the port than the one it ships (a build with
denser screen dumps, say: kept with the user's copy, outside this repository).

usage: amiga_run.py [--config a1200-030] [--frames 6000] [--name NAME]
                    [--program PATH] [--compare REFERENCE] [--music] [--show]
                    [--timeout S]
"""

import argparse
import datetime
import glob
import os
import shutil
import signal
import subprocess
import sys
import time

import oracle
import recfg

# As the port's own harness (tests/amiga/run_fsuae.sh): cycle-exact CPUs.
CONFIGS = {
    "a1200-8mb": ["--amiga_model=A1200", "--fast_memory=8192"],
    "a1200-030": ["--amiga_model=A1200", "--uae_cpu_model=68030",
                  "--uae_cpu_cycle_exact=true", "--uae_cpu_multiplier=14",
                  "--uae_cpu_24bit_addressing=false", "--zorro_iii_memory=65536"],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--config", default="a1200-030", choices=sorted(CONFIGS))
    parser.add_argument("--frames", type=int, default=6000)
    parser.add_argument("--name", default="")
    parser.add_argument("--program", help="the port's program to run (default: the one it ships)")
    parser.add_argument("--compare", metavar="REFERENCE",
                        help="checkpoints to compare with (default: the port's own)")
    parser.add_argument("--music", action="store_true", help="copy MUSIC_DAT's streams too")
    parser.add_argument("--show", action="store_true", help="show FS-UAE's window")
    parser.add_argument("--realtime", action="store_true", help="no warp mode")
    parser.add_argument("--timeout", type=int, default=1800, help="host seconds")
    args = parser.parse_args()

    cfg = recfg.load()
    original, runs, fsuae, kickstart, system = recfg.need(
        cfg, "ORIGINAL", "RUNS", "FSUAE", "KICKSTART_A1200", "AMIGA_SYSTEM")

    if not os.path.exists(os.path.join(system, "S", "Startup-Sequence")):
        sys.exit(f"{system}: no system drive, run amiga_system.py first")

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run = os.path.join(runs, f"{stamp}-amiga-{args.config}" + (f"-{args.name}" if args.name else ""))
    work = os.path.join(run, "WORK")  # FS-UAE names a directory drive after the directory
    os.makedirs(work)

    game = os.path.join(original, "binaries", "amiga")
    shutil.copy(args.program or os.path.join(game, "sz2"), os.path.join(work, "program"))
    for data in sorted(glob.glob(os.path.join(game, "*_DAT"))):
        if os.path.basename(data) == "MUSIC_DAT":
            if args.music:
                os.makedirs(os.path.join(work, "MUSIC_DAT"))
                for stream in glob.glob(os.path.join(data, "*.crm")):
                    shutil.copy(stream, os.path.join(work, "MUSIC_DAT"))
        else:
            shutil.copytree(data, os.path.join(work, os.path.basename(data)))

    # The marker is written after the program's output is closed; FailAt 21
    # keeps the script going if the program cannot be loaded.
    with open(os.path.join(work, "Run-Test"), "w", newline="\n") as out:
        out.write("FailAt 21\nCD WORK:\n"
                  f"WORK:program {args.frames} >WORK:output.txt\n"
                  "Why >>WORK:output.txt\nEcho \"done\" >WORK:marker.txt\n")

    command = [fsuae, *CONFIGS[args.config], f"--kickstart_file={kickstart}",
               f"--hard_drive_0={system}", f"--hard_drive_1={work}",
               "--floppy_drive_0=", "--fullscreen=0", "--automatic_input_grab=0",
               f"--warp_mode={0 if args.realtime else 1}", f"--base_dir={run}"]
    # SDL's offscreen driver gets no OpenGL context on macOS: FS-UAE opens a
    # window, hidden unless --show.
    env = dict(os.environ)
    if not args.show:
        command.append("--window_hidden=1")

    with open(os.path.join(run, "command.txt"), "w") as out:
        out.write(" ".join(f"'{c}'" for c in command) + "\n")

    started = time.time()
    with open(os.path.join(run, "fs-uae.log"), "w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   env=env, start_new_session=True)

    marker = os.path.join(work, "marker.txt")
    status = "TIMEOUT"
    while time.time() - started < args.timeout:
        if os.path.exists(marker):
            status = "DONE"
            break
        if process.poll() is not None:
            status = "EXITED"
            break
        time.sleep(1)

    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)

    seconds = time.time() - started
    lines = [f"status {status}", f"host seconds {seconds:.0f}"]

    output = os.path.join(work, "output.txt")
    if os.path.exists(output):
        shutil.copy(output, os.path.join(run, "output.txt"))

    checkpoints = os.path.join(work, "checkpoints.bin")
    verdict = 1
    if os.path.exists(checkpoints):
        shutil.copy(checkpoints, run)
        rows = oracle.read_checkpoints(checkpoints)
        lines.append(f"checkpoints {len(rows)}")
        # The port's own reference: 6,000 frames of the attract demo, made on
        # the Amiga; a shorter run is compared with its start.
        reference = args.compare or os.path.join(original, "tests", "amiga", "reference",
                                                 "attract-6000.txt")
        if os.path.exists(reference):
            difference = oracle.compare(oracle.read_checkpoints(reference), rows, prefix=True)
            if difference:
                lines.append("compare frame {} field {}: {} expected, {}".format(*difference))
            else:
                lines.append(f"compare MATCH {os.path.basename(reference)}")
                verdict = 0
        else:
            verdict = 0

    shots = os.path.join(run, "shots")
    for screen in sorted(glob.glob(os.path.join(work, "screen_*.bin"))):
        os.makedirs(shots, exist_ok=True)
        shutil.copy(screen, shots)
        oracle.screen_to_png(os.path.join(shots, os.path.basename(screen)))
    if os.path.isdir(shots):
        lines.append(f"screens {len(glob.glob(os.path.join(shots, '*.png')))}")

    lines.insert(0, "PASS" if status == "DONE" and verdict == 0 else "FAIL")
    with open(os.path.join(run, "result.txt"), "w") as out:
        out.write("\n".join(lines) + "\n")
    print(run)
    print("\n".join(lines))
    sys.exit(0 if lines[0] == "PASS" else 1)


if __name__ == "__main__":
    main()
