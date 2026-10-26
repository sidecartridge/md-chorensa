"""The original program as the translator sees it: the X68000 executable's
image relocated at its load address, and the disassembly's structure.

The executable (Human68k .X) gives the bytes; the disassembly (the Amiga
port's reassemblable listing of the same program) gives where the code is,
the labels and the jump tables: each of its lines from the original carries
the address and, for an instruction, its opcode words, which must equal the
image's. The port's own lines (inside its ifd __PORT__ / __AMIGA__ /
__ATARI__ blocks, without an address) are left out: this is the X68000
program as it is.
"""

import re
import struct

LOAD = 0x20000   # where the disassembly's addresses put the program


class Image:
    """text + data relocated at LOAD, then bss (zeros)."""

    def __init__(self, path):
        raw = open(path, "rb").read()
        if raw[:2] != b"HU":
            raise ValueError(f"{path}: not a Human68k executable")
        base, entry, text, data, bss, reloc, _sym = struct.unpack(">IIIIIII", raw[4:32])
        if base != 0:
            raise ValueError(f"{path}: linked at {base:#x}, not 0")
        body = bytearray(raw[64:64 + text + data])
        table = raw[64 + text + data:64 + text + data + reloc]
        offset, i, self.relocations = 0, 0, []
        while i < len(table):
            step = struct.unpack(">H", table[i:i + 2])[0]
            i += 2
            if step == 1:  # a long distance follows
                step = struct.unpack(">I", table[i:i + 4])[0]
                i += 4
            offset += step
            value = struct.unpack(">I", body[offset:offset + 4])[0]
            struct.pack_into(">I", body, offset, value + LOAD)
            self.relocations.append(LOAD + offset)
        self.text = LOAD
        self.data = LOAD + text
        self.bss = LOAD + text + data
        self.end = self.bss + bss
        self.entry = LOAD + entry
        self.bytes = bytes(body) + bytes(bss)

    def word(self, address):
        o = address - LOAD
        return struct.unpack(">H", self.bytes[o:o + 2])[0]

    def long(self, address):
        o = address - LOAD
        return struct.unpack(">I", self.bytes[o:o + 4])[0]

    def contains(self, address):
        return LOAD <= address < self.end


_INSN = re.compile(r"^\s+([a-z]+)(?:\.[bwls])?\b[^;]*;\s*([0-9A-F]{8})\s+((?:[0-9A-F]{4}\s?)+)")
_DATA = re.compile(r"^\s+(dc\.[bwl]|ds\.[bwl])\s+([^;]*?)\s*;.*?;\s*([0-9A-F]{8})\s*$")
_LABEL = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):")
_JUMPS = re.compile(r"J\(\$([0-9A-F]{8})\)")
_COND = re.compile(r"^\s+(ifd|ifnd|else|endif)\b\s*(\S*)")
_SECTION = re.compile(r"^\s+(text|data|bss)\s*$")
_DS = re.compile(r"^\s+ds\.([bwl])\s+(\$?[0-9A-Fa-f]+)")
_EVEN = re.compile(r"^\s+even\b")


class Listing:
    """The disassembly in its original form."""

    def __init__(self, path, defined=(), bss=None):
        self.instructions = {}   # address -> (mnemonic, [opcode words])
        self.data = set()        # addresses of data items
        self.labels = {}         # name -> address
        self.names = {}          # address -> name (the first label there)
        self.jump_targets = {}   # address of an instruction -> [targets it annotates]
        pending = []
        stack = []               # per open conditional: is its branch taken?
        section = "text"
        cursor = bss             # the bss's addresses: its ds sizes added up
        for line in open(path, encoding="latin-1"):
            m = _COND.match(line)
            if m:
                kind, sym = m.groups()
                if kind in ("ifd", "ifnd"):
                    stack.append((sym in defined) == (kind == "ifd"))
                elif kind == "else":
                    stack[-1] = not stack[-1]
                else:
                    stack.pop()
                continue
            if not all(stack):
                continue
            m = _SECTION.match(line)
            if m:
                section = m.group(1)
                continue
            if section == "bss" and cursor is not None:
                m = _LABEL.match(line)
                if m:
                    self.labels[m.group(1)] = cursor
                    self.names.setdefault(cursor, m.group(1))
                    continue
                m = _DS.match(line)
                if m:
                    n = m.group(2)
                    count = int(n[1:], 16) if n.startswith("$") else int(n)
                    cursor += count * {"b": 1, "w": 2, "l": 4}[m.group(1)]
                    continue
                if _EVEN.match(line):
                    cursor += cursor & 1
                continue
            m = _LABEL.match(line)
            if m:
                pending.append(m.group(1))
                continue
            address = None
            m = _INSN.match(line)
            if m:
                address = int(m.group(2), 16)
                words = [int(w, 16) for w in m.group(3).split()]
                self.instructions[address] = (m.group(1), words)
                targets = [int(t, 16) for t in _JUMPS.findall(line)]
                if targets:
                    self.jump_targets[address] = targets
            else:
                m = _DATA.match(line)
                if m:
                    address = int(m.group(3), 16)
                    self.data.add(address)
            if address is not None and pending:
                for name in pending:
                    self.labels[name] = address
                    self.names.setdefault(address, name)
                pending = []


def check(image, listing):
    """Every instruction's opcode words as the image has them: [(address,
    listed, image)] of those that differ."""
    bad = []
    for address, (_mn, words) in sorted(listing.instructions.items()):
        actual = [image.word(address + 2 * i) for i in range(len(words))]
        if actual != words:
            bad.append((address, words, actual))
    return bad
