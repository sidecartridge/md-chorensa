#!/usr/bin/env python3
"""Game-state checkpoints and screen dumps, in the Amiga port's formats.

The Amiga port, given a frame count (`sz2 6000`), writes checkpoints.bin and,
every 500 frames, screen_NNNNN.bin; our own builds of the game write the same
formats so the two can be compared.

checkpoints.bin: 'CRSC', the number of checkpoints (long), then one 32-byte
record every 60 frames: frame (long), score (long), random table index (word),
background scroll counter (word), sprites drawn (word), padding (word), the
player structure (14 bytes), padding (2 bytes). All big-endian.

screen_NNNNN.bin: 'CRSS', width, lines, colours, padding (words), 256 x R, G,
B (bytes), then width x lines palette indices.

The text form of checkpoints has one line per checkpoint:

    frame score random scroll sprites player-structure-hex

usage:
    oracle.py text CHECKPOINTS                 print as text
    oracle.py compare REFERENCE CHECKPOINTS... exit 1 at the first difference
    oracle.py png SCREEN.bin... [-o DIR]       screen dumps to PNG
"""

import argparse
import os
import struct
import sys
import zlib

FIELDS = ("frame", "score", "random", "scroll", "sprites", "player")


def read_binary(data, path):
    if data[:4] != b"CRSC":
        sys.exit(f"{path}: not a checkpoints file")

    count = struct.unpack(">I", data[4:8])[0]
    rows = []

    for index in range(count):
        record = data[8 + 32 * index:8 + 32 * (index + 1)]
        frame, score, random, scroll, sprites = struct.unpack(">IIHHH", record[:14])
        rows.append((frame, score, random, scroll, sprites, record[16:30].hex()))

    return rows


def read_text(path):
    rows = []

    for line in open(path, encoding="utf-8"):
        fields = line.split("#", 1)[0].split()

        if fields:
            rows.append(tuple(int(v) for v in fields[:5]) + (fields[5],))

    return rows


def read_checkpoints(path):
    data = open(path, "rb").read()
    return read_binary(data, path) if data[:4] == b"CRSC" else read_text(path)


def compare(reference, rows, prefix=False):
    """The first difference as (frame, field, expected, actual), or None.

    With prefix, a shorter run is compared with the start of the reference."""
    for expected, actual in zip(reference, rows):
        for name, a, b in zip(FIELDS, expected, actual):
            if a != b:
                return expected[0], name, a, b

    if len(rows) < len(reference) and not prefix:
        return reference[len(rows)][0], "missing", len(reference), len(rows)

    return None


def read_screen(path):
    data = open(path, "rb").read()

    if data[:4] != b"CRSS":
        sys.exit(f"{path}: not a screen dump")

    width, lines, colours = struct.unpack(">HHH", data[4:10])
    palette = [tuple(data[12 + 3 * i:15 + 3 * i]) for i in range(256)]
    start = 12 + 3 * 256
    pixels = data[start:start + width * lines]
    return width, lines, colours, palette, pixels


def write_png(path, width, height, rgb_rows):
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)

    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        out.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        out.write(chunk(b"IEND", b""))


def screen_to_png(path, out_dir=None):
    width, lines, _, palette, pixels = read_screen(path)
    rows = []

    for y in range(lines):
        row = bytearray()
        for index in pixels[y * width:(y + 1) * width]:
            row += bytes(palette[index])
        rows.append(row)

    base = os.path.splitext(os.path.basename(path))[0] + ".png"
    target = os.path.join(out_dir or os.path.dirname(path), base)
    write_png(target, width, lines, rows)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("text")
    p.add_argument("checkpoints")
    p = sub.add_parser("compare")
    p.add_argument("reference")
    p.add_argument("checkpoints", nargs="+")
    p = sub.add_parser("png")
    p.add_argument("screens", nargs="+")
    p.add_argument("-o", "--out")
    args = parser.parse_args()

    if args.command == "text":
        print("# frame score random scroll sprites player-structure")
        for row in read_checkpoints(args.checkpoints):
            print(" ".join(str(v) for v in row))
    elif args.command == "compare":
        reference = read_checkpoints(args.reference)
        status = 0
        for path in args.checkpoints:
            difference = compare(reference, read_checkpoints(path))
            if difference:
                frame, name, a, b = difference
                print(f"{path}: frame {frame}: {name} {a} expected, {b}")
                status = 1
            else:
                print(f"{path}: MATCH, {len(reference)} checkpoints")
        sys.exit(status)
    else:
        for path in args.screens:
            print(screen_to_png(path, args.out))


if __name__ == "__main__":
    main()
