#!/usr/bin/env python3
"""The YM2149's three channels as one 8-bit DAC on the ST: for each
unsigned 8-bit sample the volumes (vA, vB, vC) whose output, as measured on
an ST, lies nearest to a straight line over the span SPAN of the levels
(rp/src/audio.c's audioYmTriples, the 50 fps YM path).

The channels do not add: the ST ties the three outputs together into one
grounded resistor, so loud channels compress each other, and a table fitted
to the sum of single-channel levels plays out of order. The levels are Paulo
Simoes' measurements of every combination on an ST, tones and noise off
(Hatari's src/includes/ym2149_fixed_vol.h, volume_table[C][B][A]), read
from a Hatari source tree when the table is made; only the triples derived
from them are kept in this repository. The ST plays them with its mixer's
tones off (userfw.s), as they were measured.

Prints the C initialiser of audioYmTriples.

usage: ym_triples.py HATARI/src/includes/ym2149_fixed_vol.h [--span 50000]
"""

import argparse
import bisect
import re

# The span audio.c's table spreads the 256 samples over, of the 65,119 the
# three channels reach: louder than the least-error span (28,250) by 4.9 dB,
# for music about 0.6 dB more noise.
SPAN = 50000


def measured_levels(path):
    """The measured output for volumes (a, b, c): a function."""
    with open(path) as f:
        text = f.read()
    nums = [int(n) for n in re.findall(r"\d+", text[text.index("{"):])]
    if len(nums) != 16 * 16 * 16:
        raise ValueError(f"{path}: {len(nums)} values, not 4096")
    return lambda a, b, c: nums[(c * 16 + b) * 16 + a]


def fit(level, span=None):
    """(top, triples, levels): the span [0, top], the 256 triples and their
    measured levels, each the combination nearest to top * v / 255. The
    span with the least error, or `span` (louder, less exact)."""
    combos = {}
    for a in range(16):
        for b in range(a, 16):
            for c in range(b, 16):
                combos.setdefault(level(a, b, c), (c, b, a))
    levels = sorted(combos)

    def nearest(t):
        i = bisect.bisect_left(levels, t)
        return min(levels[max(0, i - 1):i + 1], key=lambda s: abs(s - t))

    best = None
    for top in [span] if span else range(8000, levels[-1] + 1, 250):
        picks = [nearest(top * v / 255) for v in range(256)]
        err = (sum((picks[v] - top * v / 255) ** 2 for v in range(256)) / 256) ** 0.5 / (top / 255)
        if best is None or err < best[0]:
            best = (err, top, picks)
    _err, top, picks = best
    return top, [combos[p] for p in picks], picks


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("table", help="Hatari's src/includes/ym2149_fixed_vol.h")
    ap.add_argument("--span", type=int, default=SPAN)
    args = ap.parse_args()
    _top, triples, _levels = fit(measured_levels(args.table), args.span)
    for row in range(0, 256, 8):
        print("    " + " ".join("{%d, %d, %d}," % t for t in triples[row:row + 8]))


if __name__ == "__main__":
    main()
