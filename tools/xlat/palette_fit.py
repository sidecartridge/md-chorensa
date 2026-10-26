#!/usr/bin/env python3
"""The follow palette scheme (tools/re/quantise.py) fitted on the X68000's
own colour entries, for the RP's renderer: which of the ST's 16 colours each
entry shows, and how the 16 follow the program's palette frame by frame.

Reads a host run's entries_NNNNN.bin (run_host.py --entries): per pixel the
entry it shows (1..255 the graphics palette's, 256 + n the sprite and text
palette's), and both palettes. The mapping is quantise.py's follow fit, by
entry. The 16 colours of a frame are what the RP can work out every time the
program changes its palette, in integers: per ST colour, the mean of its
entries' current colours in linear light, weighted by how much of the
screen each entry covers over the stretch, rounded channel by channel to the
nearest of the ST's eight levels. rp_palette() here is that same
arithmetic, so the host predicts the RP's frames exactly.

Writes palette.bin: 'CRSP', the 512 entries' ST colour (bytes; unseen
entries 0), their weights (bytes, 1..255; unseen 0), and the linear-light
table (256 x 16 bits) and the eight levels' thresholds (7 x 16 bits) the
RP's arithmetic uses. With --check: the error of that palette against the
fit's own (Oklab x 100, quantise.py's measure) over the stretch.

Its output is the game's: outside the repository, never committed.

usage: palette_fit.py RUN [--first 650] [--last 10380] [--every 10] --out FILE [--check]
"""

import argparse
import array
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "re"))
import quantise  # noqa: E402

ENTRIES = 512
SLOTS = 16
LINEAR_MAX = 4095


def colour_rgb(word):
    """The X68000's GRB555 (+ intensity, ignored) as 8-bit RGB, as the host."""
    g, r, b = (word >> 11) & 31, (word >> 6) & 31, (word >> 1) & 31
    return tuple((v << 3) | (v >> 2) for v in (r, g, b))


def load(run, first, last, every):
    frames = []
    for path in sorted(glob.glob(os.path.join(run, "entries_*.bin"))):
        number = int(re.search(r"entries_(\d+)\.bin$", path).group(1))
        if number < first or number > last or (number - first) % every:
            continue
        data = open(path, "rb").read()
        if data[:4] != b"CRSE":
            sys.exit(f"{path}: not an entries dump")
        words = struct.unpack(f">{ENTRIES}H", data[4:4 + 2 * ENTRIES])
        pixels = array.array("H")
        pixels.frombytes(data[4 + 2 * ENTRIES:])
        frames.append(quantise.Frame(number, [colour_rgb(w) for w in words], pixels.tolist()))
    if not frames:
        sys.exit(f"{run}: no entries dumps between {first} and {last}")
    return frames


LINEAR = [round(quantise._linear(v) * LINEAR_MAX) for v in range(256)]
LEVEL_LINEAR = [LINEAR[v] for v in quantise.ST_LEVELS]
# A channel's level: the first whose threshold the mean is below (the
# midpoints between the levels in linear light).
THRESHOLDS = [(LEVEL_LINEAR[k] + LEVEL_LINEAR[k + 1] + 1) // 2 for k in range(7)]


def rp_palette(mapping, weight, rgb_of):
    """The RP's 16 ST colours for the current entry colours rgb_of(entry)."""
    sums = [[0, 0, 0, 0] for _ in range(SLOTS)]
    for e in range(ENTRIES):
        w = weight[e]
        if not w:
            continue
        acc = sums[mapping[e]]
        r, g, b = rgb_of(e)
        acc[0] += w * LINEAR[r]
        acc[1] += w * LINEAR[g]
        acc[2] += w * LINEAR[b]
        acc[3] += w
    out = []
    for acc in sums:
        if not acc[3]:
            out.append((0, 0, 0))
            continue
        level = []
        for c in range(3):
            mean = acc[c] // acc[3]
            k = 0
            while k < 7 and mean >= THRESHOLDS[k]:
                k += 1
            level.append(k)
        out.append(tuple(level))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run")
    ap.add_argument("--first", type=int, default=650)
    ap.add_argument("--last", type=int, default=10380)
    ap.add_argument("--every", type=int, default=10)
    ap.add_argument("--out", required=True)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    frames = load(args.run, args.first, args.last, args.every)
    follow, _, _ = quantise.fit_follow(frames)
    w = quantise.weights(frames)
    top = max(w.values())
    mapping = [follow.mapping.get(e, 0) for e in range(ENTRIES)]
    weight = [max(1, round(255 * w[e] / top)) if e in w else 0 for e in range(ENTRIES)]
    with open(args.out, "wb") as f:
        f.write(b"CRSP" + bytes(mapping) + bytes(weight))
        f.write(struct.pack("<256H", *LINEAR) + struct.pack("<7H", *THRESHOLDS))
    used = sum(1 for v in weight if v)
    print(f"{len(frames)} frames ({frames[0].number}-{frames[-1].number}), {used} entries used, "
          f"written {args.out}")
    if args.check:
        rp = quantise.Scheme("rp", follow.mapping,
                             lambda f: rp_palette(mapping, weight, lambda e: f.palette[e]))
        for name, scheme in (("follow (Oklab fit)", follow), ("the RP's palette", rp)):
            errs = [quantise.errors(scheme, f) for f in frames]
            mean = sum(m for m, _ in errs) / len(errs)
            p99 = sorted(p for _, p in errs)[int(0.95 * len(errs))]
            print(f"  {name}: mean error {mean:.2f}, a frame's p99 at the 95th percentile {p99:.2f}")


if __name__ == "__main__":
    main()
