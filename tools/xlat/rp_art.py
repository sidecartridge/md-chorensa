#!/usr/bin/env python3
"""A stage's pictures for the RP's renderer (xl_store.h's xl_store_art_t),
from a host snapshot taken in the stage, the stage's palette fit
(palette_fit.py's palette.bin) and the patterns its sprites draw (the host's
pattern_use.bin):

  page 1   the far graphics page, every pixel's ST colour, 4 bits; where the
           page is clear, the backdrop's (the sprite palette's entry 0);
  page 0   the near page as runs of its opaque pixels, 4 bits each;
  patterns the 16 x 16 sprite patterns the stage draws, 4 bits a pixel in
           rows (the X68000 keeps the left and right halves apart), still
           the program's colour numbers: the renderer maps them through the
           sprite's palette block, and flips them as it draws;

with the entries' colours and weights and the tables the RP's palette
arithmetic uses. Written as C (xl_stage_art.c) for the stage's image.

Its output is the game's: outside the repository, never committed.

usage: rp_art.py --snapshot snapshot.bin --palette palette.bin --patterns pattern_use.bin
                 --layout xl_layout.h --out DIR
"""

import argparse
import os
import re
import struct
import sys

GVRAM = 0xC00000
GVRAM_PAGE = 0x80000
GVRAM_LINE = 1024
PAGE_LINES = 512
WIDTH = 256
STATE_WORDS = 32
SPRITE_BYTES = 512 * 8
PCG_PATTERNS = 0x75E
ENTRIES = 512
ENTRY_SPRITE = 256


def c_bytes(data):
    return "\n".join("  " + ",".join(str(b) for b in data[k:k + 24]) + ","
                     for k in range(0, len(data), 24)) or "  0,"


def c_words(values, per_line=12):
    return "\n".join("  " + ",".join(f"{v}u" for v in values[k:k + per_line]) + ","
                     for k in range(0, len(values), per_line)) or "  0,"


def page_pixels(memory, page):
    base = GVRAM + page * GVRAM_PAGE
    return [[memory[base + y * GVRAM_LINE + 2 * x + 1] for x in range(WIDTH)]
            for y in range(PAGE_LINES)]


def runs_of(row):
    """The opaque runs of a line: [(x, length)]."""
    runs, x = [], 0
    while x < WIDTH:
        if row[x]:
            start = x
            while x < WIDTH and row[x]:
                x += 1
            runs.append((start, x - start))
        else:
            x += 1
    return runs


def pattern_rows(data):
    """The X68000's 128 bytes (the left 8 pixels of row r at 4r, the right 8
    at 64 + 4r, the left pixel in the high nibble) as 16 rows of 8 bytes."""
    out = bytearray()
    for r in range(16):
        out += data[4 * r:4 * r + 4] + data[64 + 4 * r:64 + 4 * r + 4]
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--palette", required=True)
    ap.add_argument("--patterns", required=True)
    ap.add_argument("--layout", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    snap = open(args.snapshot, "rb").read()
    state = struct.unpack("<%dI" % STATE_WORDS, snap[:4 * STATE_WORDS])
    memory = snap[4 * STATE_WORDS + SPRITE_BYTES + 32 * state[31]:]
    pal = open(args.palette, "rb").read()
    if pal[:4] != b"CRSP":
        sys.exit(f"{args.palette}: not a palette fit")
    colour = pal[4:4 + ENTRIES]
    weight = pal[4 + ENTRIES:4 + 2 * ENTRIES]
    linear = struct.unpack_from("<256H", pal, 4 + 2 * ENTRIES)
    thresholds = struct.unpack_from("<7H", pal, 4 + 2 * ENTRIES + 512)
    use = open(args.patterns, "rb").read()
    if use[:4] != b"CRSU":
        sys.exit(f"{args.patterns}: not a pattern use")
    layout = {m.group(1): int(m.group(2), 16) for m in
              re.finditer(r"#define XL_(\w+) 0x([0-9A-F]+)u", open(args.layout).read())}
    patterns_at = struct.unpack_from(">I", memory, layout["SPRITE_DATA_ADDRESS"])[0] & 0xFFFFFF

    # Page 1, opaque: the backdrop's colour where it is clear.
    backdrop = colour[ENTRY_SPRITE]
    page1 = bytearray()
    for row in page_pixels(memory, 1):
        slots = [colour[g] if g else backdrop for g in row]
        page1 += bytes((slots[x] << 4) | slots[x + 1] for x in range(0, WIDTH, 2))
    # Page 0 as runs: per line their number n, n pairs (x, length - 1), then
    # each run's pixels, two a byte (xl_store.h).
    runs, index = bytearray(), []
    for row in page_pixels(memory, 0):
        index.append(len(runs))
        line = runs_of(row)
        runs.append(len(line))
        for x, n in line:
            runs += bytes((x, n - 1))
        for x, n in line:  # 4 bits a pixel, each run from a byte
            slots = [colour[g] for g in row[x:x + n]] + [0]
            runs += bytes((slots[k] << 4) | slots[k + 1] for k in range(0, n, 2))
    # The patterns the stage draws.
    pattern_index, patterns = [], bytearray()
    for p in range(PCG_PATTERNS):
        if p < len(use) - 4 and use[4 + p]:
            pattern_index.append(len(patterns) // 128)
            at = patterns_at + 128 * p
            patterns += pattern_rows(memory[at:at + 128])
        else:
            pattern_index.append(0xFFFF)

    path = os.path.join(args.out, "xl_stage_art.c")
    with open(path, "w") as f:
        f.write("/* Generated by tools/xlat/rp_art.py from the user's copy of the program:\n"
                " * never commit. */\n#include \"xl_store.h\"\n\n")
        f.write(f"static const uint8_t k_page1[{len(page1)}] = {{\n{c_bytes(page1)}\n}};\n")
        f.write(f"static const uint8_t k_page0_runs[{len(runs)}] = {{\n{c_bytes(runs)}\n}};\n")
        f.write(f"static const uint32_t k_page0_index[{PAGE_LINES}] = {{\n{c_words(index)}\n}};\n")
        f.write(f"static const uint8_t k_patterns[{max(len(patterns), 1)}] = {{\n{c_bytes(patterns)}\n}};\n")
        f.write(f"static const uint16_t k_pattern_index[{PCG_PATTERNS}] = {{\n{c_words(pattern_index)}\n}};\n")
        f.write(f"static const uint8_t k_colour[{ENTRIES}] = {{\n{c_bytes(colour)}\n}};\n")
        f.write(f"static const uint8_t k_weight[{ENTRIES}] = {{\n{c_bytes(weight)}\n}};\n")
        f.write(f"static const uint16_t k_linear[256] = {{\n{c_words(linear)}\n}};\n")
        f.write(f"static const uint16_t k_thresholds[7] = {{\n{c_words(thresholds)}\n}};\n\n")
        f.write("const xl_store_art_t xl_stage_art = {\n"
                f"  k_page1, k_page0_runs, k_page0_index, {PAGE_LINES}u,\n"
                f"  k_patterns, k_pattern_index, {PCG_PATTERNS}u,\n"
                "  k_colour, k_weight, k_linear, k_thresholds,\n};\n")
    print(f"art: page 1 {len(page1)} B, page 0 runs {len(runs)} B, "
          f"{len(patterns) // 128} patterns ({len(patterns)} B)")


if __name__ == "__main__":
    main()
