#!/usr/bin/env python3
"""The game's songs for the ST, from the Amiga port's renders of them
(MUSIC_DAT/NAME.crm: the original's MCDRV songs rendered on a YM2151, 8-bit
stereo at 22,050 Hz, intro then loop, in blocks of 4096 frames, each block
its left samples and then its right): mono signed 8-bit at one of the ST's
rates, resampled (and filtered) by ffmpeg, the loop cut so that it joins
itself and the intro without a gap. The RP plays NAME.ST8 on the STE's DMA
chip (25,033 Hz) and NAME.YM8 on the YM (7,777 Hz: --rate 7777 --ext YM8).

Writes NAME.EXT for each song: 'CRS8', the rate, the intro's and the loop's
samples (u32), the fade unit (1/10000 s per unit of MCDRV's fade speed) and
the song's level (1-64, the loudest 64) (u16), then the samples. Little
endian, as the RP reads them.

Its output is the game's: outside the repository, never committed.

usage: music_convert.py MUSIC_DAT_DIR --out DIR [--rate 25033] [--ext ST8] [NAME ...]
"""

import argparse
import glob
import os
import struct
import subprocess
import sys

HEADER = 32
RATE = 25033


def read_crm(path):
    data = open(path, "rb").read()
    if data[:4] != b"CRSM":
        sys.exit(f"{path}: not a .crm")
    _version, channels, rate, intro, loop, block = struct.unpack_from(">HHIIII", data, 4)
    _peak, fade_unit, level = struct.unpack_from(">HHH", data, 24)
    if channels != 2:
        sys.exit(f"{path}: {channels} channels")
    frames = intro + loop
    body = data[HEADER:]
    # Blocks of `block` frames, left then right: interleaved stereo.
    out = bytearray(2 * frames)
    for start in range(0, frames, block):
        n = min(block, frames - start)
        at = 2 * start
        chunk = body[at:at + 2 * n]
        out[2 * start:2 * (start + n):2] = chunk[:n]
        out[2 * start + 1:2 * (start + n):2] = chunk[n:2 * n]
    return rate, intro, loop, fade_unit, level or 64, bytes(out)


def resample(stereo, rate, out_rate):
    """Signed 8-bit stereo at `rate` to signed 8-bit mono at `out_rate`."""
    proc = subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "s8", "-ar", str(rate), "-ac", "2",
                           "-i", "-", "-ac", "1", "-ar", str(out_rate), "-f", "s8", "-"],
                          input=stereo, capture_output=True, check=True)
    return proc.stdout


def convert(path, out_dir, out_rate, ext):
    rate, intro, loop, fade_unit, level, stereo = read_crm(path)
    # Intro, then the loop three times, in one stream: the intro is cut from
    # its start, the loop from the third copy's place (the loop's end before
    # it, its start after it), so both join as they play.
    tail = stereo[2 * intro:]
    pcm = resample(stereo[:2 * intro] + tail * 3, rate, out_rate)
    intro_out = round(intro * out_rate / rate)
    loop_out = round(loop * out_rate / rate)
    start = round((intro + 2 * loop) * out_rate / rate)
    samples = pcm[:intro_out] + pcm[start:start + loop_out]
    loop_out = len(samples) - intro_out
    name = os.path.splitext(os.path.basename(path))[0]
    target = os.path.join(out_dir, f"{name}.{ext}")
    with open(target, "wb") as f:
        f.write(b"CRS8" + struct.pack("<IIIHH", out_rate, intro_out, loop_out, fade_unit, level)
                + bytes(HEADER - 20))
        f.write(samples)
    return name, intro_out, loop_out, len(samples) + HEADER


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("music_dat")
    ap.add_argument("--out", required=True)
    ap.add_argument("--rate", type=int, default=RATE)
    ap.add_argument("--ext", default="ST8")
    ap.add_argument("names", nargs="*", help="only these songs (SZ2_S1 ...)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(args.music_dat, "*.crm")))
    if args.names:
        paths = [p for p in paths if os.path.splitext(os.path.basename(p))[0] in args.names]
    total = 0
    for path in paths:
        name, intro, loop, size = convert(path, args.out, args.rate, args.ext)
        total += size
        print(f"{name}: intro {intro / args.rate:.1f} s, loop {loop / args.rate:.1f} s, {size} bytes")
    print(f"{len(paths)} songs, {total / 1e6:.1f} MB at {args.rate} Hz")


if __name__ == "__main__":
    main()
