#!/usr/bin/env python3
"""A stage's memory for the RP's flash store, from a host snapshot and the
memory map of a run resumed from it: 1 KB pages, the ones the run writes in
RAM (their contents at the snapshot copied in when the run starts), the ones
it only reads inside the stage's image (pages of 256 bytes), every 16-bit word in the RP's byte
order (xl.h's XL_PAGED accessors), and the page table as constants of the
image (xl_stage_regions: 256 regions of 64 KB, each a row of its 256 pages; the
firmware copies the regions into RAM). Pages the run never touches point at
a spare RAM page; the text plane's point into its packed area (xl_store.h's
XL_STORE_TEXT_*), its rows that are not clear at the snapshot in the image. With it: the runtime's state at the snapshot, its sprite
list and the checkpoints the host recorded, which the RP checks its own
against. tools/xlat/rp_store.py builds it into the stage's image.

Its output is the game's: it goes into a build folder outside the
repository, never committed.

usage: rp_image.py --snapshot RUN/snapshot.bin --memmap RUN/memmap.bin [--memmap ...]
                   --checkpoints RUN/checkpoints.bin --layout BUILD/xl_layout.h
                   --out DIR
"""

import argparse
import os
import re
import struct
import sys

def _store_values():
    text = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "xl_store.h")).read()
    return {m.group(1): int(m.group(2), 16) for m in
            re.finditer(r"#define (XL_STORE_\w+) 0x([0-9A-Fa-f]+)u", text)}


_STORE = _store_values()
PAGE_BITS = 8                 # xl_store.h's XL_STORE_PAGE_BITS
POOL_RAM = _STORE["XL_STORE_RAM_BYTES"] >> PAGE_BITS
POOL_WINDOW = _STORE["XL_STORE_WINDOW_FREE_BYTES"] >> PAGE_BITS
WINDOW_FREE = _STORE["XL_STORE_WINDOW_FREE"]
PAGE = 1 << PAGE_BITS
PAGES = 1 << (24 - PAGE_BITS)
# The text plane in its layout (xl_store.h, XL_STORE_TEXT_*): the screen's
# lines' first 32 bytes, in a packed area of the firmware's (xl_rp_text).
TEXT_VRAM, TEXT_PLANE, TEXT_LINE, TEXT_SHOWN = 0xE00000, 0x20000, 128, 32
TEXT_LINES = _STORE["XL_STORE_TEXT_LINES"]
TEXT_PLANE_BYTES = _STORE["XL_STORE_TEXT_PLANE_BYTES"]
TEXT_BYTES = _STORE["XL_STORE_TEXT_BYTES"]
STATE_WORDS = 32
SPRITE_BYTES = 512 * 8
MEMORY = 1 << 24

# What the runtime reads and writes for the program, by xl_layout.h's names
# (the memory map counts only the program's own accesses): bytes from each.
RUNTIME_RANGES = {
    "SPRITE_DATA_TABLE": SPRITE_BYTES,
    "CURRENT_SPRITE_DATA_ENTRY": 4,
    "PLAYER_SCORE": 4,
    "WORD_00088E6C": 2,
    "BACKGROUND_SCROLL_COUNTER": 2,
    "PLAYER_INFO_STRUCT": 14,
    "VBL_VWAIT_COUNTER": 2,
}
VBL_VECTOR = 0x118
# What the RP's renderer reads beside the game (rp/src/xl_render.c): the
# CRTC's scroll registers, both palettes and R1, read every frame; a page the
# game itself leaves alone after the snapshot would otherwise be missing.
RENDER_READS = ((0xE80000, 0x30), (0xE82000, 0x400), (0xE82500, 2))


def text_page_at(p):
    """Where the text plane's page p goes in the packed area, or None for a
    page past the screen's lines (the spare page's)."""
    plane, j = divmod(p * PAGE - TEXT_VRAM, TEXT_PLANE)
    j //= PAGE
    if j >= TEXT_LINES * TEXT_LINE // PAGE:
        return None
    return plane * TEXT_PLANE_BYTES + 32 * (j % 4) + PAGE * (j // 4)


def text_layout(memory):
    """The text plane at the snapshot in its packed layout (the bytes the
    screen shows), and each page's place in it."""
    packed = bytearray(TEXT_BYTES)
    where = {}
    for p in range(TEXT_VRAM // PAGE, (TEXT_VRAM + 4 * TEXT_PLANE) // PAGE):
        at = text_page_at(p)
        where[p] = at
        if at is None:
            continue
        for line in range(PAGE // TEXT_LINE):
            src = p * PAGE + line * TEXT_LINE
            dst = at + line * TEXT_LINE
            packed[dst:dst + TEXT_SHOWN] = memory[src:src + TEXT_SHOWN]
    for p, at in where.items():
        if at is not None and at + PAGE > TEXT_BYTES:
            sys.exit(f"the text plane's page {p:04X} runs past the packed area")
    return packed, where


def swapped(data):
    """Each 16-bit word in the RP's byte order (xl.h's XL_PAGED accessors)."""
    out = bytearray(data)
    out[0::2], out[1::2] = data[1::2], data[0::2]
    return bytes(out)


def c_bytes(data, indent="  "):
    lines = []
    for k in range(0, len(data), 24):
        lines.append(indent + ",".join(str(b) for b in data[k:k + 24]) + ",")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--memmap", required=True, action="append",
                    help="a resumed run's memory map (several: what any of them touches)")
    ap.add_argument("--checkpoints", required=True)
    ap.add_argument("--layout", required=True)
    ap.add_argument("--out", required=True)

    args = ap.parse_args()

    snap = open(args.snapshot, "rb").read()
    state = struct.unpack("<%dI" % STATE_WORDS, snap[:4 * STATE_WORDS])
    if state[0] != 0x4E534C58:
        sys.exit(f"{args.snapshot}: not a snapshot")
    at = 4 * STATE_WORDS
    sprites = snap[at:at + SPRITE_BYTES]
    at += SPRITE_BYTES + 32 * state[31]
    memory = snap[at:at + MEMORY]
    if len(memory) != MEMORY:
        sys.exit(f"{args.snapshot}: short")
    touched = bytearray(MEMORY)
    for path in args.memmap:
        data = open(path, "rb").read()
        if len(data) != MEMORY:
            sys.exit(f"{path}: not a memory map")
        touched = bytearray(a | b for a, b in zip(touched, data))
    layout = {m.group(1): int(m.group(2), 16) for m in
              re.finditer(r"#define XL_(\w+) 0x([0-9A-F]+)u", open(args.layout).read())}

    for name, size in RUNTIME_RANGES.items():
        for a in range(layout[name], layout[name] + size):
            touched[a] |= 3   # read and written by the runtime
    for a in range(VBL_VECTOR, VBL_VECTOR + 4):
        touched[a] |= 1
    for start, size in RENDER_READS:
        for a in range(start, start + size):
            touched[a] |= 1

    text_init, text_where = text_layout(memory)
    text_rows = [at for at in range(0, 4 * TEXT_PLANE_BYTES, TEXT_SHOWN)
                 if any(text_init[at:at + TEXT_SHOWN])]
    ram, flash = [], []
    for p in range(PAGES):
        if p in text_where:
            continue
        flags = 0
        for b in touched[p * PAGE:(p + 1) * PAGE]:
            flags |= b
        if flags & 6:
            ram.append(p)
        elif flags:
            flash.append(p)
    pool = POOL_RAM + POOL_WINDOW
    if len(ram) + 1 > pool:
        sys.exit(f"{len(ram)} pages written and a spare page, {pool} RAM pages to hold them")
    spare = len(ram)

    data = open(args.checkpoints, "rb").read()
    count = struct.unpack(">I", data[4:8])[0]
    checkpoints = data[8:8 + 32 * count]

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "xl_rp_image.h"), "w") as h:
        h.write("/* Generated by tools/xlat/rp_image.py from the user's copy of the program:\n"
                " * never commit. */\n#ifndef XL_RP_IMAGE_H\n#define XL_RP_IMAGE_H\n#include <stdint.h>\n")
        h.write(f"#define XL_RP_RAM_PAGES {spare + 1}u /* the written pages and a spare */\n")
        h.write(f"#define XL_RP_WRITTEN_PAGES {spare}u\n")
        h.write(f"#define XL_RP_FLASH_PAGES {len(flash)}u\n")
        h.write(f"#define XL_RP_STATE_WORDS {STATE_WORDS}u\n#define XL_RP_SPRITE_BYTES {SPRITE_BYTES}u\n")
        h.write(f"#define XL_RP_CHECKPOINTS {count}u\n")
        h.write("extern uint8_t xl_rp_ram[];  /* the firmware's RAM for the pages */\n")
        h.write("extern uint8_t xl_rp_text[];  /* the text plane's packed area */\n")
        h.write(f"#define XL_RP_TEXT_ROWS {len(text_rows)}u /* the text plane's rows not clear */\n")
        h.write("extern const uint16_t xl_rp_text_rows[];\nextern const uint8_t xl_rp_text_init[];\n")
        h.write("extern uint8_t __rom_in_ram_start__[];  /* the cartridge window */\n")
        h.write("extern const uint8_t xl_rp_ram_init[];\n")
        h.write("extern const uint32_t xl_rp_state[];\nextern const uint8_t xl_rp_sprites[];\n")
        h.write("extern const uint8_t xl_rp_checkpoints[];\n")
        h.write("extern uint8_t *const *const xl_stage_regions[256];\n")
        h.write("#endif\n")
    with open(os.path.join(args.out, "xl_rp_image.c"), "w") as c:
        c.write("/* Generated by tools/xlat/rp_image.py from the user's copy of the program:\n"
                " * never commit. */\n#include \"xl_rp_image.h\"\n\n")
        init = swapped(b"".join(memory[p * PAGE:(p + 1) * PAGE] for p in ram))
        c.write(f"const uint8_t xl_rp_ram_init[{len(init)}] __attribute__((aligned(4))) = {{\n{c_bytes(init)}\n}};\n\n")
        c.write(f"const uint16_t xl_rp_text_rows[{max(len(text_rows), 1)}] = {{\n"
                + "".join(f"  0x{at:04X}u,\n" for at in text_rows) + ("" if text_rows else "  0,\n")
                + "};\n\n")
        rows = swapped(b"".join(bytes(text_init[at:at + TEXT_SHOWN]) for at in text_rows))
        c.write(f"const uint8_t xl_rp_text_init[{max(len(rows), 1)}] __attribute__((aligned(4))) = {{\n"
                f"{c_bytes(rows) or '  0,'}\n}};\n\n")
        pages = swapped(b"".join(memory[p * PAGE:(p + 1) * PAGE] for p in flash))
        c.write(f"static const uint8_t k_flash[{max(len(pages), 1)}] __attribute__((aligned(4))) = {{\n{c_bytes(pages) or '  0,'}\n}};\n\n")
        def pool_page(k):  # the firmware's array first, then the window's arena
            if k < POOL_RAM:
                return f"xl_rp_ram + 0x{k * PAGE:X}"
            return f"__rom_in_ram_start__ + 0x{WINDOW_FREE + (k - POOL_RAM) * PAGE:X}"

        where = {}
        for k, p in enumerate(ram):
            where[p] = pool_page(k)
        for k, p in enumerate(flash):
            where[p] = f"(uint8_t *)k_flash + 0x{k * PAGE:X}"
        for p, at in text_where.items():
            if at is not None:
                where[p] = f"xl_rp_text + 0x{at:X}"
        # The table in RAM, in two levels: 256 regions of 64 KB, each a row of
        # its 64 pages; the regions the run never touches share a row of the
        # spare page.
        per_region = 1 << (16 - PAGE_BITS)
        regions = sorted({p // per_region for p in where})
        void = pool_page(spare)
        # Both levels are constants of the stage's image; the firmware's RAM
        # table (xl_regions) is copied from them.
        c.write(f"static uint8_t *const k_rows[{len(regions) + 1}][{per_region}] = {{\n")
        for r in regions:
            c.write("  {" + ", ".join(where.get(r * per_region + k, void)
                                      for k in range(per_region)) + "},\n")
        c.write("  {" + ", ".join([void] * per_region) + "},\n};\n\n")
        c.write("uint8_t *const *const xl_stage_regions[256] = {\n")
        for r in range(256):
            c.write(f"  k_rows[{regions.index(r) if r in regions else len(regions)}],\n")
        c.write("};\n\n")
        c.write(f"const uint32_t xl_rp_state[{STATE_WORDS}] = {{\n"
                + "".join(f"  0x{w:08X}u,\n" for w in state) + "};\n\n")
        c.write(f"const uint8_t xl_rp_sprites[{SPRITE_BYTES}] = {{\n{c_bytes(sprites)}\n}};\n\n")
        c.write(f"const uint8_t xl_rp_checkpoints[{max(len(checkpoints), 1)}] = {{\n"
                f"{c_bytes(checkpoints) or '  0,'}\n}};\n")
    print(f"{len(regions)} regions of 64 KB touched ({(len(regions) + 1) * per_region * 4 + 1024} "
          f"bytes of table), {len(ram)} pages in RAM ({len(ram) * PAGE // 1024} KB), "
          f"{len(flash)} in flash ({len(flash) * PAGE // 1024} KB), "
          f"{len(text_rows)} text rows not clear, "
          f"resume at frame {state[24]}, {count} checkpoints")


if __name__ == "__main__":
    main()
