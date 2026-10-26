#!/usr/bin/env python3
"""A static translator from the X68000 program to C (tools/xlat).

Reads the user's copy of the program: the executable for its bytes, the
disassembly for where the code is (program.py), decodes every instruction
(m68k.py) and writes C over a flat big-endian memory (xl.h): one C function
per entry point (the program's entry, every call target, every code address
the program keeps as a pointer), the 68000's stack kept in that memory, the
flags computed only where something may read them.

A call pushes the real return address and calls the callee's function, which
returns the address its RTS popped: the call site goes on only if that is
the address it pushed, else it passes it up, so code that rewrites the stack
lands where the 68000 would. A jump to another entry is a C tail call, a
jump through a register a call by address (xl_call); the switch tables'
targets are labels of their function.

Its output is the game's own code in another language: it goes to a build
folder outside the repository (never committed).

usage: xlat.py --executable SZ2.X --listing sz2.s --out DIR [--stats]
"""

import argparse
import os
import re
import sys

import m68k
import program

FLAG_N, FLAG_Z, FLAG_V, FLAG_C, FLAG_X = 1, 2, 4, 8, 16
ALL_FLAGS = 31
NZVC = FLAG_N | FLAG_Z | FLAG_V | FLAG_C
COND_USES = {
    "t": 0, "f": 0, "hi": FLAG_C | FLAG_Z, "ls": FLAG_C | FLAG_Z, "cc": FLAG_C, "cs": FLAG_C,
    "ne": FLAG_Z, "eq": FLAG_Z, "vc": FLAG_V, "vs": FLAG_V, "pl": FLAG_N, "mi": FLAG_N,
    "ge": FLAG_N | FLAG_V, "lt": FLAG_N | FLAG_V, "gt": FLAG_N | FLAG_V | FLAG_Z,
    "le": FLAG_N | FLAG_V | FLAG_Z,
}
COND_C = {
    "t": "1", "f": "0", "hi": "(!xl.c && !xl.z)", "ls": "(xl.c || xl.z)", "cc": "!xl.c",
    "cs": "xl.c", "ne": "!xl.z", "eq": "xl.z", "vc": "!xl.v", "vs": "xl.v", "pl": "!xl.n",
    "mi": "xl.n", "ge": "(xl.n == xl.v)", "lt": "(xl.n != xl.v)",
    "gt": "(!xl.z && xl.n == xl.v)", "le": "(xl.z || xl.n != xl.v)",
}
# Hardware the runtime must see when the program reads or writes it at an
# absolute address: the MFP (GPIP: the vertical blank), the joystick port
# (the PPI), the CRTC (its raster registers).
HOOKED = [(0xE80000, 0xE80030), (0xE88000, 0xE88030), (0xE9A000, 0xE9A008)]


# Where the runtime takes over from the program, at the same points as the
# Amiga port's own changes (the oracle was recorded with them): the frame
# waits, the sprite list's hand-over, two routines that need hardware a
# flat memory does not have (the CRTC's simultaneous text-plane access, the
# FLOAT library's sine and cosine), and the CPU benchmark. Each: the C to run
# in place of the original instructions at that address, then where to go
# on ('goto', address) or 'rts'.
PATCHES = {
    0x2020A: ("xl_patch_xsp_vsync();", ("goto", 0x2021A)),     # XSP_VSYNC's wait
    0x204D0: ("xl_patch_xsp_out();", "rts"),                    # XSP_OUT: the sprite list
    0x225E2: ("xl_patch_clear_text_planes();", "rts"),          # CLEAR_TEXT_PLANE
    0x226FE: ("", ("goto", 0x22718)),                           # a raster sync on the MFP
    0x22C98: ("xl_patch_wait_vbl();", ("goto", 0x22CB4)),      # WAIT_VBL's wait
    0x277DC: ("D(0) = 1000u;", "rts"),                          # CALCULATE_CPU_POWER
    0x29222: ("xl_patch_sincos();", ("goto", 0x29408)),        # the sine and cosine tables
}
# Code the runtime runs just before an original instruction, which then runs
# as usual: the port decodes the ADPCM samples there (and rewrites the game's
# sample table, whose lengths the effects' timing then uses).
PATCHES_BEFORE = {
    0x29422: "xl_patch_samples_loaded();",   # before LOAD_RANDOM_TABLE
}
# The program's variables the runtime reads or writes, by the listing's names.
EXPORTS = ("PCM_SAMPLE_INFO_TABLE", "PLAYER_SCORE", "WORD_00088E6C", "BACKGROUND_SCROLL_COUNTER", "PLAYER_INFO_STRUCT",
           "VBL_VWAIT_COUNTER", "SPRITE_DATA_TABLE", "CURRENT_SPRITE_DATA_ENTRY", "NEW_STACK",
           "SIN_COS_TABLES_START", "SPRITE_DATA_ADDRESS")


def hooked(address):
    a = address & 0xFFFFFF
    return any(lo <= a < hi for lo, hi in HOOKED)


# --- What each operation reads and sets of the flags ---------------------------

def flag_effect(i):
    """(uses, defines) of the flags."""
    op = i.op
    if op in ("bcc", "scc", "dbcc"):
        return COND_USES[i.cond], 0
    if op in ("add", "addi", "addq", "sub", "subi", "subq", "neg"):
        if i.dst[0] == "a":
            return 0, 0
        return 0, ALL_FLAGS
    if op in ("addx", "subx", "negx"):
        return FLAG_X | FLAG_Z, ALL_FLAGS
    if op in ("cmp", "cmpi", "cmpa", "cmpm"):
        return 0, NZVC
    if op in ("move", "moveq", "and", "andi", "or", "ori", "eor", "eori", "not", "tst", "clr",
              "ext", "swap", "muls", "mulu", "tas"):
        if op == "move" and i.dst[0] == "ccr":
            return 0, ALL_FLAGS
        if op == "move" and i.dst[0] == "sr":
            return 0, ALL_FLAGS
        if op == "move" and i.src[0] == "sr":
            return ALL_FLAGS, 0
        if op in ("andi", "ori", "eori") and i.dst[0] in ("ccr", "sr"):
            return ALL_FLAGS, ALL_FLAGS
        return 0, NZVC
    if op in ("divu", "divs"):
        return 0, NZVC
    if op in ("asl", "asr", "lsl", "lsr"):
        return 0, ALL_FLAGS
    if op in ("rol", "ror"):
        return 0, NZVC
    if op in ("roxl", "roxr"):
        return FLAG_X, ALL_FLAGS
    if op in ("btst", "bset", "bclr", "bchg"):
        return 0, FLAG_Z
    if op in ("chk",):
        return 0, FLAG_N
    if op == "trap":
        return 0, 0                   # IOCS: the RTE gives the flags back
    if op == "patch":
        return 0, ALL_FLAGS
    if op in ("jsr", "bsr", "linef", "stop"):
        return ALL_FLAGS, ALL_FLAGS   # the callee may read them, and sets them
    if op in ("rts", "rte"):
        return ALL_FLAGS, 0           # the caller may read them
    if op == "jmp":
        return ALL_FLAGS, 0
    return 0, 0


# --- The program's functions ---------------------------------------------------

class Program:
    def __init__(self, image, listing, resume=None):
        self.image = image
        self.listing = listing
        self.insns = {}
        for a in listing.instructions:
            self.insns[a] = m68k.decode(image, a)
        self.code = set(self.insns)
        self.discovered = set()  # instructions the listing does not place
        self._jump_tables()
        self._entries()
        if resume:
            self._resume_entries(resume)

    def _jump_tables(self):
        """Each JMP through a switch table, `jmp table(pc, Dn)`: the table's
        word offsets from its base, read from the image up to the first
        target (where the table ends and the code begins)."""
        self.jump_targets = {}
        for a, i in self.insns.items():
            if i.op != "jmp" or i.target is not None or i.dst[0] != "pcidx":
                continue
            base = i.dst[1]
            if base in self.listing.instructions:
                # A jump into unrolled code: a negated count indexes back from
                # the base, onto any instruction between the JMP and it.
                targets, at = [], a + i.length
                while at <= base:
                    targets.append(at)
                    at += m68k.decode(self.image, at).length
                if targets[-1] != base:
                    raise SystemExit(f"{a:06X}: unrolled code does not end at {base:06X}")
                self.jump_targets[a] = targets
                continue
            targets, end, k = [], None, 0
            while k < 512:
                at = base + 2 * k
                if end is not None and at >= end:
                    break
                off = self.image.word(at)
                t = (base + (off - 0x10000 if off & 0x8000 else off)) & 0xFFFFFFFF
                if not (self.image.text <= t < self.image.data) or t & 1:
                    break
                targets.append(t)
                if t > base:
                    end = t if end is None else min(end, t)
                k += 1
            self.jump_targets[a] = sorted(set(targets))

    def _entries(self):
        e = {self.image.entry}
        for i in self.insns.values():
            if i.flow == "call" and i.target is not None and i.target in self.code:
                e.add(i.target)
            for operand in (i.src, i.dst):
                if operand and operand[0] in ("pcdisp",) and i.op in ("lea", "pea") \
                        and operand[1] in self.code:
                    e.add(operand[1])
        for r in self.image.relocations:
            v = self.image.long(r)
            if v in self.code:
                e.add(v)
        self.entries = e

    def _resume_entries(self, path):
        """Entries a run resumed from a snapshot returns into (the host's
        resume.txt): `ADDRESS` where it starts, `?ADDRESS` a long from the
        68000 stack, an entry when it follows a call (others are data)."""
        after_call = {a + i.length for a, i in self.insns.items() if i.flow == "call"}
        for line in open(path):
            word = line.split("#")[0].strip()
            if not word:
                continue
            if word.startswith("?"):
                a = int(word[1:], 16)
                if a in after_call:
                    self.entries.add(a)
            else:
                a = int(word, 16)
                if a not in self.code:
                    sys.exit(f"{path}: {a:06X} is not an instruction")
                self.entries.add(a)

    def function(self, entry):
        """The instructions reachable from `entry` without leaving through a
        call or into another entry: {address: insn}."""
        seen, work = {}, [entry]
        while work:
            a = work.pop()
            if a in seen:
                continue
            if a in PATCHES:
                code, then = PATCHES[a]
                p = m68k.Insn(a)
                p.op, p.extra = "patch", (code, then)
                if then == "rts":
                    p.flow = "return"
                else:
                    p.flow, p.target = "jump", then[1]
                    if then[1] not in self.entries or then[1] == entry:
                        work.append(then[1])
                seen[a] = p
                continue
            i = self.insn(a, entry)
            seen[a] = i
            nxt = a + i.length
            for t in self.successors(i):
                if t == entry or t not in self.entries:
                    work.append(t)
            if i.flow in ("next", "call", "branch") and nxt not in self.entries:
                work.append(nxt)
            elif i.flow in ("next", "call", "branch") and nxt == entry:
                work.append(nxt)
        return seen

    def insn(self, a, entry):
        """The instruction at `a`, decoded from the image when the listing
        has none there (its address comments are not always right)."""
        i = self.insns.get(a)
        if i is None:
            if not (self.image.text <= a < self.image.data) or a & 1:
                raise SystemExit(f"function {entry:06X}: flow reaches {a:06X}, outside the code")
            try:
                i = m68k.decode(self.image, a)
            except m68k.DecodeError as e:
                raise SystemExit(f"function {entry:06X}: {a:06X} does not decode ({e})")
            self.insns[a] = i
            self.code.add(a)
            self.discovered.add(a)
        return i

    def successors(self, i):
        """The static targets inside a function: branches, jumps, tables."""
        if i.op == "patch":
            return [i.target] if i.target is not None else []
        if i.flow == "branch":
            return [i.target]
        if i.flow == "jump":
            if i.target is not None:
                return [i.target]
            return list(self.jump_targets.get(i.address, []))
        return []


# --- Flag liveness ---------------------------------------------------------------

def liveness(prog, insns):
    """{address: the flags live after it}."""
    succ = {}
    for a, i in insns.items():
        s = []
        nxt = a + i.length
        if i.flow in ("next", "call", "branch"):
            s.append(nxt)
        s += prog.successors(i)
        succ[a] = s
    live_out = {a: 0 for a in insns}
    live_in = {a: 0 for a in insns}
    effects = {a: flag_effect(i) for a, i in insns.items()}
    changed = True
    while changed:
        changed = False
        for a in sorted(insns, reverse=True):
            out = 0
            for t in succ[a]:
                out |= live_in[t] if t in insns else ALL_FLAGS  # into another function
            uses, defs = effects[a]
            inn = uses | (out & ~defs)
            if out != live_out[a] or inn != live_in[a]:
                live_out[a], live_in[a] = out, inn
                changed = True
    return live_out


# --- C emission -------------------------------------------------------------------

MASK = {1: "M8", 2: "M16", 4: "M32"}
BITS = {1: 8, 2: 16, 4: 32}
RD = {1: "rd8", 2: "rd16", 4: "rd32"}
WR = {1: "wr8", 2: "wr16", 4: "wr32"}


class Emitter:
    def __init__(self, prog):
        self.prog = prog
        self.lines = []
        self.tmp = 0

    def t(self):
        self.tmp += 1
        return f"t{self.tmp}"

    def out(self, s):
        self.lines.append(s)

    # Effective addresses: (address expression, or None for a register)
    def address(self, op, size):
        kind = op[0]
        if kind == "ind":
            return f"A({op[1]})"
        if kind == "post":
            v = self.t()
            step = 2 if (op[1] == 7 and size == 1) else size
            self.out(f"  uint32_t {v} = A({op[1]}); A({op[1]}) += {step};")
            return v
        if kind == "pre":
            step = 2 if (op[1] == 7 and size == 1) else size
            self.out(f"  A({op[1]}) -= {step};")
            v = self.t()
            self.out(f"  uint32_t {v} = A({op[1]});")
            return v
        if kind == "disp":
            return f"(A({op[1]}) + (uint32_t){op[2]})"
        if kind == "idx":
            _, n, d, xk, m, xs = op
            x = f"{xk.upper()}({m})" if xs == 4 else f"sx16({xk.upper()}({m}))"
            return f"(A({n}) + (uint32_t){d} + {x})"
        if kind == "abs":
            return f"0x{op[1] & 0xFFFFFFFF:08X}u"
        if kind == "pcdisp":
            return f"0x{op[1]:08X}u"
        if kind == "pcidx":
            _, base, xk, m, xs = op
            x = f"{xk.upper()}({m})" if xs == 4 else f"sx16({xk.upper()}({m}))"
            return f"(0x{base:08X}u + {x})"
        raise ValueError(op)

    def read(self, op, size, addr=None):
        """A C expression of the operand's value (zero-extended to size)."""
        kind = op[0]
        if kind == "d":
            return f"(D({op[1]}) & {MASK[size]})"
        if kind == "a":
            return f"(A({op[1]}) & {MASK[size]})"
        if kind == "imm":
            return f"0x{op[1] & (0xFFFFFFFF if size == 4 else (1 << (8 * size)) - 1):X}u"
        if addr is None:
            addr = self.address(op, size)
        if kind == "abs" and hooked(op[1]):
            return f"xl_hw_rd({addr}, {size})"
        return f"{RD[size]}({addr})"

    def write(self, op, size, value, addr=None):
        kind = op[0]
        if kind == "d":
            if size == 4:
                self.out(f"  D({op[1]}) = {value};")
            else:
                m = MASK[size]
                self.out(f"  D({op[1]}) = (D({op[1]}) & ~{m}) | (({value}) & {m});")
            return
        if kind == "a":
            self.out(f"  A({op[1]}) = {value};")
            return
        if addr is None:
            addr = self.address(op, size)
        if kind == "abs" and hooked(op[1]):
            self.out(f"  xl_hw_wr({addr}, {value}, {size});")
            return
        self.out(f"  {WR[size]}({addr}, {value});")

    def nz(self, value, size, live):
        bits = BITS[size]
        if live & FLAG_N:
            self.out(f"  xl.n = ({value} >> {bits - 1}) & 1u;")
        if live & FLAG_Z:
            self.out(f"  xl.z = ({value} & {MASK[size]}) == 0;")

    def vc0(self, live):
        if live & FLAG_V:
            self.out("  xl.v = 0;")
        if live & FLAG_C:
            self.out("  xl.c = 0;")

    # Operations ----------------------------------------------------------------
    def emit(self, i, live, fname):
        op, s = i.op, i.size
        nxt = i.address + i.length
        e = getattr(self, "op_" + op, None)
        if e is None:
            raise SystemExit(f"{i.address:06X}: no translation for {op}")
        e(i, s, live, nxt, fname)

    def op_move(self, i, s, live, nxt, fn):
        if i.dst[0] == "ccr":
            v = self.t()
            self.out(f"  uint32_t {v} = {self.read(i.src, 2)};")
            self.out(f"  xl.c = {v} & 1u; xl.v = ({v} >> 1) & 1u; xl.z = ({v} >> 2) & 1u; "
                     f"xl.n = ({v} >> 3) & 1u; xl.x = ({v} >> 4) & 1u;")
            return
        if i.dst[0] == "sr":
            self.out(f"  xl_set_sr({self.read(i.src, 2)});")
            return
        if i.src[0] == "sr":
            self.write(i.dst, 2, "xl_get_sr()")
            return
        v = self.t()
        self.out(f"  uint32_t {v} = {self.read(i.src, s)};")
        self.write(i.dst, s, v)
        self.nz(v, s, live)
        self.vc0(live)

    def op_movea(self, i, s, live, nxt, fn):
        v = self.read(i.src, s)
        self.out(f"  A({i.dst[1]}) = {('sx16(' + v + ')') if s == 2 else v};")

    def op_moveq(self, i, s, live, nxt, fn):
        v = i.src[1] & 0xFFFFFFFF
        self.out(f"  D({i.dst[1]}) = 0x{v:08X}u;")
        if live & FLAG_N:
            self.out(f"  xl.n = {1 if v & 0x80000000 else 0};")
        if live & FLAG_Z:
            self.out(f"  xl.z = {1 if v == 0 else 0};")
        self.vc0(live)

    def _addsub(self, i, s, live, sub, store=True, xflag=True):
        if i.dst[0] == "a":  # ADDA/SUBA/ADDQ/SUBQ to An: 32 bits, no flags
            src = self.read(i.src, s)
            if s == 2:
                src = f"sx16({src})"
            self.out(f"  A({i.dst[1]}) {'-' if sub else '+'}= {src};")
            return
        sv, dv, rv = self.t(), self.t(), self.t()
        self.out(f"  uint32_t {sv} = {self.read(i.src, s)};")
        addr = None
        if i.dst[0] not in ("d", "a"):
            addr = self.t()
            self.out(f"  uint32_t {addr} = {self.address(i.dst, s)};")
        self.out(f"  uint32_t {dv} = {self.read(i.dst, s, addr)};")
        m, b = MASK[s], BITS[s]
        if sub:
            self.out(f"  uint32_t {rv} = ({dv} - {sv}) & {m};")
        else:
            self.out(f"  uint32_t {rv} = ({dv} + {sv}) & {m};")
        if store:
            self.write(i.dst, s, rv, addr)
        self.nz(rv, s, live)
        if live & FLAG_V:
            if sub:
                self.out(f"  xl.v = ((({sv} ^ {dv}) & ({rv} ^ {dv})) >> {b - 1}) & 1u;")
            else:
                self.out(f"  xl.v = ((~({sv} ^ {dv}) & ({sv} ^ {rv})) >> {b - 1}) & 1u;")
        if live & (FLAG_C | FLAG_X):
            if sub:
                cexp = f"({sv} > {dv})"
            else:
                cexp = f"({rv} < {sv})"
            if live & FLAG_C:
                self.out(f"  xl.c = {cexp};")
            if xflag and live & FLAG_X:
                self.out(f"  xl.x = {cexp};")

    def op_add(self, i, s, live, nxt, fn):
        self._addsub(i, s, live, False)

    op_addi = op_addq = op_adda = op_add

    def op_sub(self, i, s, live, nxt, fn):
        self._addsub(i, s, live, True)

    op_subi = op_subq = op_suba = op_sub

    def op_cmp(self, i, s, live, nxt, fn):
        if i.dst[0] == "a":  # CMPA: 32 bits, the source sign-extended
            sv, dv, rv = self.t(), self.t(), self.t()
            src = self.read(i.src, s)
            self.out(f"  uint32_t {sv} = {('sx16(' + src + ')') if s == 2 else src};")
            self.out(f"  uint32_t {dv} = A({i.dst[1]});")
            self.out(f"  uint32_t {rv} = {dv} - {sv};")
            self.nz(rv, 4, live)
            if live & FLAG_V:
                self.out(f"  xl.v = ((({sv} ^ {dv}) & ({rv} ^ {dv})) >> 31) & 1u;")
            if live & FLAG_C:
                self.out(f"  xl.c = {sv} > {dv};")
            return
        self._addsub(i, s, live, True, store=False, xflag=False)

    op_cmpi = op_cmpa = op_cmpm = op_cmp

    def op_neg(self, i, s, live, nxt, fn):
        addr = None
        if i.dst[0] not in ("d", "a"):
            addr = self.t()
            self.out(f"  uint32_t {addr} = {self.address(i.dst, s)};")
        dv, rv = self.t(), self.t()
        m, b = MASK[s], BITS[s]
        self.out(f"  uint32_t {dv} = {self.read(i.dst, s, addr)};")
        self.out(f"  uint32_t {rv} = (0u - {dv}) & {m};")
        self.write(i.dst, s, rv, addr)
        self.nz(rv, s, live)
        if live & FLAG_V:
            self.out(f"  xl.v = (({dv} & {rv}) >> {b - 1}) & 1u;")
        if live & FLAG_C:
            self.out(f"  xl.c = {rv} != 0;")
        if live & FLAG_X:
            self.out(f"  xl.x = {rv} != 0;")

    def _logic(self, i, s, live, cop):
        if i.dst[0] in ("ccr", "sr"):
            v = self.read(i.src, 2)
            if i.dst[0] == "sr":
                self.out(f"  xl_set_sr(xl_get_sr() {cop} {v});")
            else:
                self.out(f"  {{ uint32_t c = (xl_get_sr() & 0xFF00u) | ((xl_get_sr() {cop} {v}) & 0xFFu); "
                         f"xl_set_sr(c); }}")
            return
        sv, dv, rv = self.t(), self.t(), self.t()
        self.out(f"  uint32_t {sv} = {self.read(i.src, s)};")
        addr = None
        if i.dst[0] not in ("d", "a"):
            addr = self.t()
            self.out(f"  uint32_t {addr} = {self.address(i.dst, s)};")
        self.out(f"  uint32_t {dv} = {self.read(i.dst, s, addr)};")
        self.out(f"  uint32_t {rv} = ({dv} {cop} {sv}) & {MASK[s]};")
        self.write(i.dst, s, rv, addr)
        self.nz(rv, s, live)
        self.vc0(live)

    def op_and(self, i, s, live, nxt, fn):
        self._logic(i, s, live, "&")

    op_andi = op_and

    def op_or(self, i, s, live, nxt, fn):
        self._logic(i, s, live, "|")

    op_ori = op_or

    def op_eor(self, i, s, live, nxt, fn):
        self._logic(i, s, live, "^")

    op_eori = op_eor

    def op_not(self, i, s, live, nxt, fn):
        addr = None
        if i.dst[0] not in ("d", "a"):
            addr = self.t()
            self.out(f"  uint32_t {addr} = {self.address(i.dst, s)};")
        rv = self.t()
        self.out(f"  uint32_t {rv} = ~{self.read(i.dst, s, addr)} & {MASK[s]};")
        self.write(i.dst, s, rv, addr)
        self.nz(rv, s, live)
        self.vc0(live)

    def op_clr(self, i, s, live, nxt, fn):
        self.write(i.dst, s, "0u")
        if live & FLAG_N:
            self.out("  xl.n = 0;")
        if live & FLAG_Z:
            self.out("  xl.z = 1;")
        self.vc0(live)

    def op_tst(self, i, s, live, nxt, fn):
        v = self.t()
        self.out(f"  uint32_t {v} = {self.read(i.dst, s)};")
        self.nz(v, s, live)
        self.vc0(live)

    def op_ext(self, i, s, live, nxt, fn):
        n = i.dst[1]
        v = self.t()
        if s == 2:
            self.out(f"  uint32_t {v} = sx8(D({n})) & M16;")
            self.out(f"  D({n}) = (D({n}) & ~M16) | {v};")
        else:
            self.out(f"  uint32_t {v} = sx16(D({n}));")
            self.out(f"  D({n}) = {v};")
        self.nz(v, s, live)
        self.vc0(live)

    def op_swap(self, i, s, live, nxt, fn):
        n = i.dst[1]
        self.out(f"  D({n}) = (D({n}) << 16) | (D({n}) >> 16);")
        self.nz(f"D({n})", 4, live)
        self.vc0(live)

    def op_exg(self, i, s, live, nxt, fn):
        a = f"{i.src[0].upper()}({i.src[1]})"
        b = f"{i.dst[0].upper()}({i.dst[1]})"
        self.out(f"  {{ uint32_t t = {a}; {a} = {b}; {b} = t; }}")

    def op_lea(self, i, s, live, nxt, fn):
        self.out(f"  A({i.dst[1]}) = {self.address(i.src, 4)};")

    def op_pea(self, i, s, live, nxt, fn):
        v = self.t()
        self.out(f"  uint32_t {v} = {self.address(i.src, 4)};")
        self.out(f"  A(7) -= 4; wr32(A(7), {v});")

    def op_link(self, i, s, live, nxt, fn):
        n = i.dst[1]
        self.out(f"  A(7) -= 4; wr32(A(7), A({n})); A({n}) = A(7); A(7) += (uint32_t){i.src[1]};")

    def op_unlk(self, i, s, live, nxt, fn):
        n = i.dst[1]
        self.out(f"  A(7) = A({n}); A({n}) = rd32(A(7)); A(7) += 4;")

    def op_movem(self, i, s, live, nxt, fn):
        if i.src[0] == "list":  # registers to memory
            mask, predec = i.src[1], i.src[2]
            ea = i.dst
            regs = [("D" if k < 8 else "A") + f"({k % 8})" for k in range(16)]
            if predec:
                n = ea[1]
                # bit 0 = A7 ... bit 15 = D0; stored from the highest address down
                order = [regs[15 - k] for k in range(16) if mask & (1 << k)]
                self.out(f"  {{ uint32_t p = A({n});")
                for r in order:
                    val = r if r != f"A({n})" else "A(" + str(n) + ")"
                    self.out(f"    p -= {s}; {WR[s]}(p, {val});")
                self.out(f"    A({n}) = p; }}")
            else:
                base = self.address(ea, s)
                self.out(f"  {{ uint32_t p = {base};")
                for k in range(16):
                    if mask & (1 << k):
                        self.out(f"    {WR[s]}(p, {regs[k]}); p += {s};")
                self.out("  }")
            return
        mask = i.dst[1]
        ea = i.src
        regs = [("D" if k < 8 else "A") + f"({k % 8})" for k in range(16)]
        if ea[0] == "post":
            n = ea[1]
            self.out(f"  {{ uint32_t p = A({n});")
        else:
            self.out(f"  {{ uint32_t p = {self.address(ea, s)};")
        for k in range(16):
            if mask & (1 << k):
                v = f"{RD[s]}(p)" if s == 4 else f"sx16(rd16(p))"
                self.out(f"    {regs[k]} = {v}; p += {s};")
        if ea[0] == "post":
            self.out(f"    A({ea[1]}) = p;")
        self.out("  }")

    def op_mulu(self, i, s, live, nxt, fn):
        v = self.t()
        n = i.dst[1]
        self.out(f"  uint32_t {v} = (D({n}) & M16) * {self.read(i.src, 2)};")
        self.out(f"  D({n}) = {v};")
        self.nz(v, 4, live)
        self.vc0(live)

    def op_muls(self, i, s, live, nxt, fn):
        v = self.t()
        n = i.dst[1]
        self.out(f"  uint32_t {v} = (uint32_t)((int32_t)(int16_t)D({n}) * "
                 f"(int32_t)(int16_t){self.read(i.src, 2)});")
        self.out(f"  D({n}) = {v};")
        self.nz(v, 4, live)
        self.vc0(live)

    def op_divu(self, i, s, live, nxt, fn):
        d, n = self.t(), i.dst[1]
        self.out(f"  uint32_t {d} = {self.read(i.src, 2)};")
        self.out(f"  if ({d} == 0) xl_divide_by_zero(0x{i.address:06X}u);")
        self.out(f"  else {{ uint32_t q = D({n}) / {d}, r = D({n}) % {d};")
        self.out("    if (q > 0xFFFFu) { xl.v = 1; xl.c = 0; }")
        self.out(f"    else {{ D({n}) = (r << 16) | q; xl.n = (q >> 15) & 1u; xl.z = q == 0; "
                 "xl.v = 0; xl.c = 0; } }")

    def op_divs(self, i, s, live, nxt, fn):
        d, n = self.t(), i.dst[1]
        self.out(f"  int32_t {d} = (int16_t){self.read(i.src, 2)};")
        self.out(f"  if ({d} == 0) xl_divide_by_zero(0x{i.address:06X}u);")
        self.out(f"  else {{ int32_t q = (int32_t)D({n}) / {d}, r = (int32_t)D({n}) % {d};")
        self.out("    if (q > 32767 || q < -32768) { xl.v = 1; xl.c = 0; }")
        self.out(f"    else {{ D({n}) = (((uint32_t)r & M16) << 16) | ((uint32_t)q & M16); "
                 "xl.n = q < 0; xl.z = q == 0; xl.v = 0; xl.c = 0; } }")

    def _shift(self, i, s, live, kind, left):
        count = i.src
        cnt = f"{count[1]}u" if count[0] == "imm" else f"(D({count[1]}) & 63u)"
        addr = None
        if i.dst[0] != "d":
            addr = self.t()
            self.out(f"  uint32_t {addr} = {self.address(i.dst, s)};")
        v = self.t()
        self.out(f"  uint32_t {v} = xl_shift({kind}, {1 if left else 0}, "
                 f"{self.read(i.dst, s, addr)}, {cnt}, {BITS[s]});")
        self.write(i.dst, s, v, addr)

    def op_asl(self, i, s, live, nxt, fn): self._shift(i, s, live, 0, True)
    def op_asr(self, i, s, live, nxt, fn): self._shift(i, s, live, 0, False)
    def op_lsl(self, i, s, live, nxt, fn): self._shift(i, s, live, 1, True)
    def op_lsr(self, i, s, live, nxt, fn): self._shift(i, s, live, 1, False)
    def op_roxl(self, i, s, live, nxt, fn): self._shift(i, s, live, 2, True)
    def op_roxr(self, i, s, live, nxt, fn): self._shift(i, s, live, 2, False)
    def op_rol(self, i, s, live, nxt, fn): self._shift(i, s, live, 3, True)
    def op_ror(self, i, s, live, nxt, fn): self._shift(i, s, live, 3, False)

    def _bit(self, i, s, live, action):
        bits = 32 if i.dst[0] == "d" else 8
        bit = f"{i.src[1] % bits}u" if i.src[0] == "imm" else f"(D({i.src[1]}) & {bits - 1}u)"
        addr = None
        if i.dst[0] != "d":
            addr = self.t()
            self.out(f"  uint32_t {addr} = {self.address(i.dst, 1)};")
        size = 4 if i.dst[0] == "d" else 1
        v, b = self.t(), self.t()
        self.out(f"  uint32_t {b} = {bit};")
        self.out(f"  uint32_t {v} = {self.read(i.dst, size, addr)};")
        if live & FLAG_Z:
            self.out(f"  xl.z = (({v} >> {b}) & 1u) == 0;")
        if action:
            expr = {"set": f"{v} | (1u << {b})", "clr": f"{v} & ~(1u << {b})",
                    "chg": f"{v} ^ (1u << {b})"}[action]
            self.write(i.dst, size, expr, addr)

    def op_btst(self, i, s, live, nxt, fn): self._bit(i, s, live, None)
    def op_bset(self, i, s, live, nxt, fn): self._bit(i, s, live, "set")
    def op_bclr(self, i, s, live, nxt, fn): self._bit(i, s, live, "clr")
    def op_bchg(self, i, s, live, nxt, fn): self._bit(i, s, live, "chg")

    def op_scc(self, i, s, live, nxt, fn):
        self.write(i.dst, 1, f"({COND_C[i.cond]} ? 0xFFu : 0u)")

    def op_nop(self, i, s, live, nxt, fn):
        pass

    def op_stop(self, i, s, live, nxt, fn):
        self.out(f"  xl_stop(0x{i.src[1]:04X}u, 0x{nxt:06X}u);")

    def op_trap(self, i, s, live, nxt, fn):
        self.out(f"  xl_trap({i.extra}u, 0x{i.address:06X}u);")

    def op_linef(self, i, s, live, nxt, fn):
        self.out(f"  xl_linef(0x{i.extra:04X}u, 0x{i.address:06X}u);")

    def op_move_usp(self, i, s, live, nxt, fn):
        pass  # one stack throughout: the program's supervisor switches are no-ops

    # Control flow -------------------------------------------------------------
    def goto(self, target, fn):
        """A jump to `target` from function `fn`'s body."""
        if target in fn.body and target != fn.entry:
            return f"goto L_{target:06X};"
        if target == fn.entry:
            return f"goto L_{target:06X};"
        if target in self.prog.entries:
            return f"return f_{target:06X}();"
        raise SystemExit(f"{fn.entry:06X}: jump to {target:06X}, neither local nor an entry")

    def call(self, target_expr, ret, fn, static_target=None):
        call = f"f_{static_target:06X}()" if static_target is not None else f"xl_call({target_expr})"
        self.out(f"  A(7) -= 4; wr32(A(7), 0x{ret:06X}u);")
        self.out(f"  {{ uint32_t r = {call}; if (r != 0x{ret:06X}u) return r; }}")

    def op_bcc(self, i, s, live, nxt, fn):
        self.out(f"  if ({COND_C[i.cond]}) {self.goto(i.target, fn)}")

    def op_dbcc(self, i, s, live, nxt, fn):
        n = i.dst[1]
        self.out(f"  if (!{COND_C[i.cond]}) {{ uint32_t w = (D({n}) - 1u) & M16; "
                 f"D({n}) = (D({n}) & ~M16) | w; if (w != M16) {self.goto(i.target, fn)} }}")

    def op_bra(self, i, s, live, nxt, fn):
        self.out(f"  {self.goto(i.target, fn)}")

    def op_bsr(self, i, s, live, nxt, fn):
        if i.target not in self.prog.entries:
            raise SystemExit(f"{i.address:06X}: call to {i.target:06X}, not an entry")
        self.call(None, nxt, fn, i.target)

    def op_jsr(self, i, s, live, nxt, fn):
        if i.target is not None and i.target in self.prog.entries:
            self.call(None, nxt, fn, i.target)
            return
        v = self.t()
        self.out(f"  uint32_t {v} = {self.address(i.dst, 4)};")
        self.call(v, nxt, fn)

    def op_jmp(self, i, s, live, nxt, fn):
        if i.target is not None:
            self.out(f"  {self.goto(i.target, fn)}")
            return
        v = self.t()
        self.out(f"  uint32_t {v} = {self.address(i.dst, 4)};")
        cases = [t for t in self.prog.jump_targets.get(i.address, []) if t in fn.body]
        if cases:
            self.out(f"  switch ({v}) {{")
            for t in cases:
                self.out(f"    case 0x{t:06X}u: goto L_{t:06X};")
            self.out("    default: break;")
            self.out("  }")
        self.out(f"  return xl_call({v});")

    def op_patch(self, i, s, live, nxt, fn):
        code, then = i.extra
        if code:
            self.out(f"  {code}")
        if then == "rts":
            self.op_rts(i, s, live, nxt, fn)
        else:
            self.out(f"  {self.goto(then[1], fn)}")

    def op_rts(self, i, s, live, nxt, fn):
        self.out(f"  {{ uint32_t r = rd32(A(7)); A(7) += 4; XL_RETURN(0x{i.address:06X}u, r); return r; }}")

    def op_rte(self, i, s, live, nxt, fn):
        self.out("  { xl_set_sr(rd16(A(7))); uint32_t r = rd32(A(7) + 2); A(7) += 6; return r; }")


class Function:
    def __init__(self, entry, body):
        self.entry = entry
        self.body = body


def translate(prog, out_dir, files=32, only=None):
    os.makedirs(out_dir, exist_ok=True)
    entries = sorted(prog.entries)
    functions = []
    for e in entries:
        functions.append(Function(e, prog.function(e)))
    names = prog.listing.names
    stats = {"functions": len(functions), "instructions": sum(len(f.body) for f in functions)}
    per_file = (len(functions) + files - 1) // files
    decls = [f"uint32_t f_{f.entry:06X}(void);" for f in functions]
    with open(os.path.join(out_dir, "xl_functions.h"), "w") as h:
        h.write("/* Generated by tools/xlat: the translated program's functions. */\n")
        h.write("#include <stdint.h>\n" + "\n".join(decls) + "\n")
    c_lines = 0
    for k in range(files):
        chunk = functions[k * per_file:(k + 1) * per_file]
        em = Emitter(prog)
        em.out("/* Generated by tools/xlat from the user's copy of the program: never commit. */")
        em.out('#include "xl.h"')
        em.out('#include "xl_functions.h"')
        em.out("#pragma GCC diagnostic ignored \"-Wunused-label\"")
        em.out("#pragma GCC diagnostic ignored \"-Wunused-variable\"")
        for f in chunk:
            live = liveness(prog, f.body)
            label = names.get(f.entry, "")
            em.out("")
            em.out(f"/* {label} */" if label else "")
            if only is not None and f.entry not in only:
                # Left out (--only): reaching it is an error the runtime reports.
                em.out(f"uint32_t f_{f.entry:06X}(void) {{")
                em.out(f'  xl_fatal("left out of this translation", 0x{f.entry:06X}u);')
                em.out("  return 0;")
                em.out("}")
                continue
            em.out(f"uint32_t f_{f.entry:06X}(void) {{")
            em.out(f"  XL_ENTER(0x{f.entry:06X}u);")
            order = sorted(f.body)
            if order[0] != f.entry:  # code before the entry, reached by branches
                em.out(f"  goto L_{f.entry:06X};")
            for n, a in enumerate(order):
                i = f.body[a]
                em.out(f" L_{a:06X}:")
                if a in PATCHES_BEFORE:
                    em.out(f"  {PATCHES_BEFORE[a]}")
                em.out("  {")
                em.emit(i, live[a], f)
                em.out("  }")
                nxt = a + i.length
                falls = i.flow in ("next", "call", "branch")
                following = order[n + 1] if n + 1 < len(order) else None
                if falls and following != nxt:
                    em.out(f"  {em.goto(nxt, f)}")
            em.out("}")
        with open(os.path.join(out_dir, f"xl_code_{k:02d}.c"), "w") as c:
            c.write("\n".join(em.lines) + "\n")
        c_lines += len(em.lines)
    # The dispatch table: every entry by address.
    with open(os.path.join(out_dir, "xl_dispatch.c"), "w") as c:
        c.write("/* Generated by tools/xlat: entries by address. */\n")
        c.write('#include "xl.h"\n#include "xl_functions.h"\n')
        c.write("typedef uint32_t (*xl_fn_t)(void);\n")
        c.write("const struct { uint32_t address; xl_fn_t fn; } xl_entries[] = {\n")
        for f in functions:
            c.write(f"  {{0x{f.entry:06X}u, f_{f.entry:06X}}},\n")
        c.write("};\n")
        c.write(f"const uint32_t xl_entry_count = {len(functions)}u;\n")
    stats["c_lines"] = c_lines
    return stats


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--executable", required=True)
    ap.add_argument("--listing", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--resume", help="the entries of a resumed run (the host's resume.txt)")
    ap.add_argument("--only", help="translate only these entries (a coverage.txt), the others "
                    "as stubs that report themselves")
    args = ap.parse_args()
    image = program.Image(args.executable)
    listing = program.Listing(args.listing, bss=image.bss)
    bad = program.check(image, listing)
    if bad:
        sys.exit(f"{len(bad)} instructions of the listing differ from the executable")
    prog = Program(image, listing, args.resume)
    only = None
    if args.only:
        only = {int(line.split()[0], 16) for line in open(args.only) if not line.startswith("#")}
    stats = translate(prog, args.out, only=only)
    with open(os.path.join(args.out, "xl_image.bin"), "wb") as f:
        f.write(image.bytes[:image.bss - program.LOAD])
    with open(os.path.join(args.out, "xl_layout.h"), "w") as h:
        h.write("/* Generated by tools/xlat: where the program sits in memory. */\n")
        h.write(f"#define XL_LOAD 0x{program.LOAD:06X}u\n#define XL_BSS 0x{image.bss:06X}u\n"
                f"#define XL_END 0x{image.end:06X}u\n#define XL_ENTRY 0x{image.entry:06X}u\n")
        for name in EXPORTS:
            if name not in listing.labels:
                sys.exit(f"the listing has no {name}")
            h.write(f"#define XL_{name} 0x{listing.labels[name]:06X}u\n")
    print(f"{len(prog.discovered)} instructions found by the flow that the listing misplaces: "
          + " ".join(f"{a:06X}" for a in sorted(prog.discovered)[:20]))
    print(f"{stats['functions']} functions, {stats['instructions']} instructions emitted "
          f"(with duplicates), {stats['c_lines']} lines of C in {args.out}")


if __name__ == "__main__":
    main()
