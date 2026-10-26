#!/usr/bin/env python3
"""The RP's renderer (rp/src/xl_render.c, built into the host by run_host.py
--rp-art) against the host's own picture through the palette scheme: for
every frame with both rpscreen_NNNNN.bin and entries_NNNNN.bin, each pixel's
ST colour must be its entry's (palette_fit.py's mapping) and the 16 colours
palette_fit.py's arithmetic on that frame's palettes. Prints the frames
that differ and how; PNGs of the first few (the host's, the RP's, the
pixels that differ).

usage: render_check.py RUN --palette palette.bin
"""

import argparse
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "re"))
import oracle  # noqa: E402
import palette_fit  # noqa: E402
import quantise  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run")
    ap.add_argument("--palette", required=True)
    args = ap.parse_args()
    pal = open(args.palette, "rb").read()
    mapping, weight = list(pal[4:516]), list(pal[516:1028])
    same = bad = 0
    for path in sorted(glob.glob(os.path.join(args.run, "rpscreen_*.bin"))):
        n = int(re.search(r"rpscreen_(\d+)\.bin$", path).group(1))
        entries = os.path.join(args.run, f"entries_{n:05d}.bin")
        if not os.path.exists(entries):
            continue
        frame = palette_fit.load(args.run, n, n, 1)[0]
        _w, _l, _c, rgb, pixels = oracle.read_screen(path)
        want_pal = [quantise.st_rgb(s) for s in
                    palette_fit.rp_palette(mapping, weight, lambda e: frame.palette[e])]
        want = bytes(mapping[e] for e in frame.pixels)
        pixel_diff = [k for k in range(len(want)) if want[k] != pixels[k]]
        palette_diff = [k for k in range(16) if tuple(rgb[k]) != want_pal[k]]
        if not pixel_diff and not palette_diff:
            same += 1
            continue
        bad += 1
        print(f"frame {n}: {len(pixel_diff)} pixels differ"
              + (f" (first at {pixel_diff[0] % 256},{pixel_diff[0] // 256}: "
                 f"{pixels[pixel_diff[0]]} for {want[pixel_diff[0]]}, entry "
                 f"{frame.pixels[pixel_diff[0]]})" if pixel_diff else "")
              + (f", palette entries {palette_diff}" if palette_diff else ""))
        if bad <= 3:
            def rows(idx, p):
                return [b"".join(bytes(p[idx[y * 256 + x]]) for x in range(256)) for y in range(256)]
            stem = os.path.join(args.run, f"render-{n:05d}")
            oracle.write_png(stem + "-host.png", 256, 256, rows(want, want_pal))
            oracle.write_png(stem + "-rp.png", 256, 256, rows(pixels, [tuple(c) for c in rgb]))
            mask = bytearray(256 * 256)
            for k in pixel_diff:
                mask[k] = 1
            oracle.write_png(stem + "-diff.png", 256, 256, rows(mask, [(0, 0, 0), (255, 0, 255)]))
    print(f"{same} frames identical, {bad} differ")
    return 1 if bad or not same else 0


if __name__ == "__main__":
    sys.exit(main())
