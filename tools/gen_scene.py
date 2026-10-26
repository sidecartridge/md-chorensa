#!/usr/bin/env python3
"""The bench's synthetic scene, built to the numbers of the busiest frames of
the game this firmware ports, from a fixed seed (nothing of the game itself):

- two background pages of 256 x 512 pixels: page 1 opaque, ten colours;
  page 0 over it, about 57% opaque in three or four runs a line;
- 452 sprite patterns of 16 x 16 (the number a stage uses), from bullets to
  ship-sized shapes;
- each in the formats the bench compares: 4 bits a pixel packed (two pixels
  a byte, the left one in the high nibble, 0 transparent), 8 bits a pixel,
  and run lists (the opaque runs of each line and their pixels).

Writes one C header of const arrays (they stay in flash on the RP), included
by rp/src/bench_scene.c. The RP's CMake build runs this; so does
tools/re/hatari_run.py --build for EmuMD.

usage: gen_scene.py --out FILE
"""

import argparse
import os
import random

WIDTH = 256
PAGE_LINES = 512
PATTERNS = 452
PATTERN_SIZE = 16
SEED = 0x5EED

# The scene's 16 ST colours (0RRR0GGG0BBB): page 1's darks, page 0's
# browns and greys, the sprites' brights.
PALETTE = [0x000, 0x001, 0x112, 0x113, 0x223, 0x224, 0x011, 0x122, 0x233, 0x344,
           0x321, 0x432, 0x543, 0x700, 0x770, 0x777]
PAGE1_COLOURS = range(1, 11)
PAGE0_COLOURS = range(6, 13)


def page1(rng):
    """Opaque: a grid of 32 x 32 cells, each a frame and a few traces."""
    page = [[0] * WIDTH for _ in range(PAGE_LINES)]
    for cy in range(0, PAGE_LINES, 32):
        for cx in range(0, WIDTH, 32):
            base = rng.choice(PAGE1_COLOURS[:4])
            edge = rng.choice(PAGE1_COLOURS[4:])
            for y in range(32):
                for x in range(32):
                    c = base
                    if x in (0, 31) or y in (0, 31):
                        c = edge
                    elif (x + y) % 9 == 0 and rng.random() < 0.5:
                        c = rng.choice(PAGE1_COLOURS)
                    page[cy + y][cx + x] = c
    return page


def page0(rng):
    """About 57% opaque in three or four runs a line: columns that wander."""
    page = [[0] * WIDTH for _ in range(PAGE_LINES)]
    runs = 3 + (rng.random() < 0.5)
    columns = []
    for n in range(4):
        columns.append([n * 64 + rng.randrange(0, 12), rng.randrange(36, 46)])
    for y in range(PAGE_LINES):
        if y % 16 == 0:
            runs = 3 + (rng.random() < 0.5)
        for n, col in enumerate(columns[:runs]):
            col[0] = max(n * 64, min(n * 64 + 20, col[0] + rng.choice((-1, 0, 0, 1))))
            width = col[1] + (8 if runs == 3 else 0)
            colour = PAGE0_COLOURS[(y // 8 + n) % len(PAGE0_COLOURS)]
            for x in range(col[0], min(WIDTH, col[0] + width)):
                page[y][x] = colour if (x - col[0]) % 7 else PAGE0_COLOURS[-1]
    return page


def pattern(rng, n):
    """A 16 x 16 shape: small bullets, round shots, ships (mirrored)."""
    p = [[0] * PATTERN_SIZE for _ in range(PATTERN_SIZE)]
    kind = n % 4
    if kind == 0:   # a bullet: a small capsule
        w, h = rng.randrange(2, 5), rng.randrange(5, 11)
        x0, y0 = (16 - w) // 2, (16 - h) // 2
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                p[y][x] = 15 if abs(x - 7.5) < 1 else 14
    elif kind == 1:  # a round shot
        r = rng.uniform(3.0, 7.5)
        for y in range(16):
            for x in range(16):
                d = ((x - 7.5) ** 2 + (y - 7.5) ** 2) ** 0.5
                if d < r:
                    p[y][x] = 15 if d < r / 2 else 13 + (n % 2)
    else:            # a ship or a piece of one: random, mirrored
        for y in range(16):
            for x in range(8):
                if rng.random() < 0.35 + 0.4 * (1 - abs(y - 8) / 8) * (x / 8):
                    c = rng.randrange(1, 16)
                    p[y][x + 8] = c
                    p[y][7 - x] = c
    return p


def packed4(rows):
    out = bytearray()
    for row in rows:
        for x in range(0, len(row), 2):
            out.append((row[x] << 4) | row[x + 1])
    return out


def runs_of(row):
    """The opaque runs of a line: [(x, length)]."""
    runs, x = [], 0
    while x < len(row):
        if row[x]:
            start = x
            while x < len(row) and row[x]:
                x += 1
            runs.append((start, x - start))
        else:
            x += 1
    return runs


def page_runs(page):
    """Per line: the number of runs, then each run's x and length - 1, then
    the runs' pixels (8 bits); an index of each line's offset."""
    data, index = bytearray(), []
    for row in page:
        index.append(len(data))
        runs = runs_of(row)
        data.append(len(runs))
        for x, n in runs:
            data += bytes((x, n - 1))
        for x, n in runs:
            data += bytes(row[x:x + n])
    return data, index


def pattern_runs(p):
    """Per line: the number of runs, then each run as (x << 4 | length - 1)
    followed by its pixels."""
    out = bytearray()
    for row in p:
        runs = runs_of(row)
        out.append(len(runs))
        for x, n in runs:
            out.append((x << 4) | (n - 1))
            out += bytes(row[x:x + n])
    return out


def c_array(name, ctype, values, per_line=24):
    lines = [f"static const {ctype} {name}[{len(values)}] __attribute__((aligned(4))) = {{"]
    fmt = "0x{:02X}" if ctype == "uint8_t" else "{}u"
    for i in range(0, len(values), per_line):
        lines.append("  " + ", ".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    rng = random.Random(SEED)
    p1, p0 = page1(rng), page0(rng)
    pats = [pattern(rng, n) for n in range(PATTERNS)]
    p0_runs, p0_index = page_runs(p0)
    rle, rle_index = bytearray(), []
    for p in pats:
        rle_index.append(len(rle))
        rle += pattern_runs(p)
    opaque = sum(1 for row in p0 for v in row if v) / (WIDTH * PAGE_LINES)
    runs_a_line = sum(len(runs_of(row)) for row in p0) / PAGE_LINES
    sprite_opaque = sum(1 for p in pats for row in p for v in row if v) / (PATTERNS * 256)

    parts = [
        "/* Generated by tools/gen_scene.py: the bench's synthetic scene. Do not edit. */",
        "#ifndef BENCH_SCENE_ASSETS_H",
        "#define BENCH_SCENE_ASSETS_H",
        "#include <stdint.h>",
        f"#define SCENE_PAGE_LINES {PAGE_LINES}",
        f"#define SCENE_PATTERNS {PATTERNS}",
        f"/* page 0: {100 * opaque:.0f}% opaque, {runs_a_line:.1f} runs a line; "
        f"sprites {100 * sprite_opaque:.0f}% opaque */",
        c_array("scene_palette", "uint16_t", PALETTE),
        c_array("scene_page1_4", "uint8_t", packed4(p1)),
        c_array("scene_page0_4", "uint8_t", packed4(p0)),
        c_array("scene_page0_runs", "uint8_t", p0_runs),
        c_array("scene_page0_index", "uint32_t", p0_index),
        c_array("scene_patterns_4", "uint8_t", b"".join(packed4(p) for p in pats)),
        c_array("scene_patterns_8", "uint8_t",
                bytes(v for p in pats for row in p for v in row)),
        c_array("scene_patterns_rle", "uint8_t", rle),
        c_array("scene_patterns_rle_index", "uint32_t", rle_index),
        "#endif",
        "",
    ]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    text = "\n".join(parts)
    if os.path.exists(args.out) and open(args.out).read() == text:
        return 0
    with open(args.out, "w") as out:
        out.write(text)
    print(f"{args.out}: page 0 {100 * opaque:.0f}% opaque, {runs_a_line:.1f} runs a line, "
          f"sprites {100 * sprite_opaque:.0f}% opaque")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
