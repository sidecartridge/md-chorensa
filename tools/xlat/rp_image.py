#!/usr/bin/env python3
"""A scene's memory for the RP's flash store (tools/xlat/xl_store.h), as C
for its image (xl_rp_image.c): pages of 256 bytes, every 16-bit word in the
RP's byte order (xl.h's XL_PAGED accessors), and the page table as constants
of the image (xl_stage_regions: 256 regions of 64 KB, each a row of its 256
pages; the firmware copies the regions into RAM). The pages the scene writes
are in RAM (the pool, xl_store.h), the ones it only reads in its own image or
in COMMON's (xl_common_pages: the ones more than one scene reads), the text
plane's in its packed area (XL_STORE_TEXT_*); every other page points at a
spare RAM page. tools/xlat/rp_store.py chooses the pages and builds the C
into the images. For the scene the game starts in, the snapshot's state
too: the runtime's state words, its sprite list and the checkpoints the
host recorded, which the RP checks its own against, in the image; the RAM
pages' and the text plane's contents in the start file (write_start).

Its output is the game's: it goes into a build folder outside the
repository, never committed.
"""

import os
import re
import struct
import sys


def _store_values():
    text = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "xl_store.h")).read()
    return {m.group(1): int(m.group(2), 16) for m in
            re.finditer(r"#define (XL_STORE_\w+) 0x([0-9A-Fa-f]+)u", text)}


_STORE = _store_values()
_VERSION = int(re.search(r"#define XL_STORE_VERSION (\d+)u", open(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "xl_store.h")).read()).group(1))
PAGE_BITS = 8                 # xl_store.h's XL_STORE_PAGE_BITS
POOL_RAM = _STORE["XL_STORE_RAM_BYTES"] >> PAGE_BITS
POOL_WINDOW = _STORE["XL_STORE_WINDOW_FREE_BYTES"] >> PAGE_BITS
WINDOW_FREE = _STORE["XL_STORE_WINDOW_FREE"]
POOL_FB = _STORE["XL_STORE_FB_FREE_BYTES"] >> PAGE_BITS
FB_FREE = _STORE["XL_STORE_FB_FREE"]
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
    return "\n".join(indent + ",".join(str(b) for b in data[k:k + 24]) + ","
                     for k in range(0, len(data), 24))


def runtime_pages(layout):
    """The pages the runtime touches for the program, which the memory maps
    leave out: (in RAM, read). The renderer's (the palettes, the CRTC's
    scrolls, R1) are in RAM in every scene: it reads them every frame, and
    what one scene writes there another shows."""
    written, read = set(), set()
    for name, size in RUNTIME_RANGES.items():
        written |= set(range(layout[name] // PAGE, (layout[name] + size - 1) // PAGE + 1))
    read |= {VBL_VECTOR // PAGE}
    for start, size in RENDER_READS:
        written |= set(range(start // PAGE, (start + size - 1) // PAGE + 1))
    return written, read


def text_pages():
    return range(TEXT_VRAM // PAGE, (TEXT_VRAM + 4 * TEXT_PLANE) // PAGE)


def pool_page(k):
    """The pool's page k (xl_store.h): the chunky buffer's part, the
    firmware's array, the window's arena."""
    if k < POOL_FB:
        return f"fb_chunked_buffer + 0x{FB_FREE + k * PAGE:X}"
    k -= POOL_FB
    if k < POOL_RAM:
        return f"xl_rp_ram + 0x{k * PAGE:X}"
    return f"__rom_in_ram_start__ + 0x{WINDOW_FREE + (k - POOL_RAM) * PAGE:X}"


POOL = POOL_FB + POOL_RAM + POOL_WINDOW


SPARE = POOL - 1  # the pool's last slot: every page no scene touches
OWNER = SPARE - _STORE["XL_STORE_OWNER_SLOTS"]  # below it: which page each slot holds


def assign_slots(ram):
    """One pool slot a page across every scene ({scene: pages} in RAM): the
    pages most scenes keep first, each in the lowest slot no page of its
    scenes holds yet. ({page: slot}, the pages whose slot another page shares)."""
    scenes_of = {}
    for scene, pages in ram.items():
        for p in pages:
            scenes_of.setdefault(p, set()).add(scene)
    used = {scene: set() for scene in ram}
    slot = {}
    for p in sorted(scenes_of, key=lambda p: (-len(scenes_of[p]), p)):
        taken = set().union(*(used[s] for s in scenes_of[p]))
        k = 0
        while k in taken:
            k += 1
        if k >= OWNER:
            sys.exit(f"the scenes' RAM pages need more than the pool's {OWNER} slots")
        slot[p] = k
        for s in scenes_of[p]:
            used[s].add(k)
    holders = {}
    for p, k in slot.items():
        holders.setdefault(k, []).append(p)
    return slot, {p for k, ps in holders.items() if len(ps) > 1 for p in ps}


def write_image(out, memory, ram, own, common_at, boot, entry, bench=None):
    """xl_rp_image.h and .c in `out`: `ram` the pages in RAM {page: slot},
    `own` the read-only pages in the scene's image, `common_at` {page: its
    offset in COMMON's xl_common_pages}; `entry` the pages set on entry to
    their content in `boot` (the memory after the boot) when their slot holds
    another page. `bench`, for the logic bench's stage: {"state": words, "sprites":
    bytes, "checkpoints": bytes}, with the RAM pages and the text plane as
    `memory` holds them. Returns a line of figures."""
    own = sorted(own)
    # The pages set on entry first (xl_store.h's ram_list).
    order = sorted(ram, key=lambda p: (p not in entry, p))
    entries = sum(1 for p in order if p in entry)
    _text_init, text_where = text_layout(memory)
    count = len(bench["checkpoints"]) // 32 if bench else 0
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "xl_rp_image.h"), "w") as h:
        h.write("/* Generated by tools/xlat/rp_image.py from the user's copy of the program:\n"
                " * never commit. */\n#ifndef XL_RP_IMAGE_H\n#define XL_RP_IMAGE_H\n#include \"xl_store.h\"\n")
        h.write(f"#define XL_RP_RAM_COUNT {len(order)}u\n#define XL_RP_ENTRY_COUNT {entries}u\n")
        h.write("extern const xl_store_ram_page_t xl_rp_ram_list[];\nextern const uint8_t xl_rp_entry_init[];\n")
        h.write(f"#define XL_RP_FLASH_PAGES {len(own)}u\n")
        h.write(f"#define XL_RP_STATE_WORDS {STATE_WORDS}u\n#define XL_RP_SPRITE_BYTES {SPRITE_BYTES}u\n")
        h.write(f"#define XL_RP_CHECKPOINTS {count}u\n")
        h.write("extern uint8_t xl_rp_ram[];  /* the firmware's RAM for the pages */\n")
        h.write("extern uint8_t xl_rp_text[];  /* the text plane's packed area */\n")
        h.write("extern uint8_t __rom_in_ram_start__[];  /* the cartridge window */\n")
        h.write("extern uint8_t fb_chunked_buffer[];  /* its part a banded frame leaves */\n")
        h.write("extern const uint8_t xl_common_pages[];  /* COMMON's read-only pages */\n")
        h.write("extern uint8_t *const *const xl_stage_regions[256];\n")
        if bench:
            h.write("extern const uint32_t xl_rp_state[];\nextern const uint8_t xl_rp_sprites[];\n")
            h.write("extern const uint8_t xl_rp_checkpoints[];\n")
        h.write("#endif\n")
    with open(os.path.join(out, "xl_rp_image.c"), "w") as c:
        c.write("/* Generated by tools/xlat/rp_image.py from the user's copy of the program:\n"
                " * never commit. */\n#include \"xl_rp_image.h\"\n\n")
        c.write(f"const xl_store_ram_page_t xl_rp_ram_list[{max(len(order), 1)}] = {{\n"
                + "".join(f"  {{0x{p:04X}u, {ram[p]}u}},\n" for p in order) + "};\n\n")
        entry = swapped(b"".join(boot[p * PAGE:(p + 1) * PAGE] for p in order[:entries]))
        c.write(f"const uint8_t xl_rp_entry_init[{max(len(entry), 1)}] __attribute__((aligned(4))) = "
                f"{{\n{c_bytes(entry) or '  0,'}\n}};\n\n")
        if bench:
            c.write(f"const uint32_t xl_rp_state[{STATE_WORDS}] = {{\n"
                    + "".join(f"  0x{w:08X}u,\n" for w in bench["state"]) + "};\n\n")
            c.write(f"const uint8_t xl_rp_sprites[{SPRITE_BYTES}] = {{\n{c_bytes(bench['sprites'])}\n}};\n\n")
            c.write(f"const uint8_t xl_rp_checkpoints[{max(len(bench['checkpoints']), 1)}] = {{\n"
                    f"{c_bytes(bench['checkpoints']) or '  0,'}\n}};\n\n")
        pages = swapped(b"".join(memory[p * PAGE:(p + 1) * PAGE] for p in own))
        c.write(f"static const uint8_t k_flash[{max(len(pages), 1)}] __attribute__((aligned(4))) = "
                f"{{\n{c_bytes(pages) or '  0,'}\n}};\n\n")
        where = {}
        for p, k in ram.items():
            where[p] = pool_page(k)
        for k, p in enumerate(own):
            where[p] = f"(uint8_t *)k_flash + 0x{k * PAGE:X}"
        for p, at in common_at.items():
            where.setdefault(p, f"(uint8_t *)xl_common_pages + 0x{at:X}")
        for p, at in text_where.items():
            if at is not None:
                where[p] = f"xl_rp_text + 0x{at:X}"
        # Two levels: 256 regions of 64 KB, each a row of its pages; the
        # regions the scene never touches share a row of the spare page. Both
        # are constants of the image; the firmware's RAM table (xl_regions) is
        # copied from them.
        per_region = 1 << (16 - PAGE_BITS)
        regions = sorted({p // per_region for p in where})
        void = pool_page(SPARE)
        c.write(f"static uint8_t *const k_rows[{len(regions) + 1}][{per_region}] = {{\n")
        for r in regions:
            c.write("  {" + ", ".join(where.get(r * per_region + k, void)
                                      for k in range(per_region)) + "},\n")
        c.write("  {" + ", ".join([void] * per_region) + "},\n};\n\n")
        c.write("uint8_t *const *const xl_stage_regions[256] = {\n")
        for r in range(256):
            c.write(f"  k_rows[{regions.index(r) if r in regions else len(regions)}],\n")
        c.write("};\n")
    return order, (f"{len(ram)} pages in RAM ({entries} set on entry), "
            f"{len(own)} read-only own ({len(own) * PAGE // 1024} KB), "
            f"{sum(1 for p in common_at if p in where and 'common' in where[p])} in COMMON, "
            f"{len(regions)} regions ({(len(regions) + 1) * per_region * 4 // 1024} KB of table)"
            + (f", {count} checkpoints" if bench else ""))


def write_start(path, memory, order, kind, firmware_id):
    """The start's memory (xl_store.h's xl_store_start_t, XL_STORE_START_FILE):
    the start scene's RAM pages in `order` (its image's ram_list) and the text
    plane's rows that are not clear, as `memory` holds them."""
    text_init, _where = text_layout(memory)
    rows = [at for at in range(0, 4 * TEXT_PLANE_BYTES, TEXT_SHOWN)
            if any(text_init[at:at + TEXT_SHOWN])]
    with open(path, "wb") as f:
        f.write(struct.pack("<II", _STORE["XL_STORE_START_MAGIC"], _VERSION))
        f.write(firmware_id.encode().ljust(32, b"\0")[:32])
        f.write(struct.pack("<III", kind, len(order), len(rows)))
        f.write(swapped(b"".join(memory[p * PAGE:(p + 1) * PAGE] for p in order)))
        offsets = struct.pack(f"<{len(rows)}H", *rows)
        f.write(offsets + b"\0" * (-len(offsets) % 4))
        f.write(swapped(b"".join(bytes(text_init[at:at + TEXT_SHOWN]) for at in rows)))
    return len(order), len(rows)


def read_snapshot(path):
    """(state words, sprite list, memory) of a host snapshot."""
    snap = open(path, "rb").read()
    state = struct.unpack("<%dI" % STATE_WORDS, snap[:4 * STATE_WORDS])
    if state[0] != 0x4E534C58:
        sys.exit(f"{path}: not a snapshot")
    at = 4 * STATE_WORDS
    sprites = snap[at:at + SPRITE_BYTES]
    at += SPRITE_BYTES + 32 * state[31]
    memory = snap[at:at + MEMORY]
    if len(memory) != MEMORY:
        sys.exit(f"{path}: short")
    return state, sprites, memory
