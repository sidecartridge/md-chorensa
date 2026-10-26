"""A 68000 decoder: an instruction's operation, size, operands and control
flow, from the image's words. Covers the 68000's user instruction set and the
few supervisor ones a program uses (moves to and from SR, ORI/ANDI to SR,
RTE), and the X68000's line-F system calls: $FFxx (Human68k DOS) and $FExx
(FLOAT), two bytes each, like a TRAP.

Operands are tuples:
  ('d', n) ('a', n)                    data / address register
  ('ind', n) ('post', n) ('pre', n)    (An) (An)+ -(An)
  ('disp', n, d)                       d16(An)
  ('idx', n, d, kind, m, size)         d8(An, Xm.size): kind 'd' or 'a', size 2 or 4
  ('abs', address)                     abs.w (sign-extended) or abs.l
  ('pcdisp', address)                  d16(PC): the address it names
  ('pcidx', address, kind, m, size)    d8(PC, Xm): the base address it names
  ('imm', value)
  ('sr',) ('ccr',) ('usp',)
  ('list', mask, predec)               MOVEM's register list (bit 0 = D0 .. bit 15 = A7)
"""

CONDITIONS = ("t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
              "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le")
SIZES = {0: 1, 1: 2, 2: 4}


class DecodeError(Exception):
    pass


class Insn:
    def __init__(self, address):
        self.address = address
        self.length = 2
        self.op = None
        self.size = None
        self.src = None
        self.dst = None
        self.cond = None      # Bcc / DBcc / Scc
        self.target = None    # a branch's or call's static target
        self.flow = "next"    # next, branch, jump, call, return, rte
        self.extra = None     # TRAP number, line-F word

    def __repr__(self):
        return f"<{self.address:06X} {self.op}.{self.size} {self.src} {self.dst}>"


class _Reader:
    def __init__(self, image, address):
        self.image = image
        self.pc = address + 2

    def word(self):
        w = self.image.word(self.pc)
        self.pc += 2
        return w

    def sword(self):
        w = self.word()
        return w - 0x10000 if w & 0x8000 else w

    def long(self):
        hi = self.word()
        return (hi << 16) | self.word()


def _s8(v):
    return v - 0x100 if v & 0x80 else v


def _ea(r, mode, reg, size):
    """An effective address operand."""
    if mode == 0:
        return ("d", reg)
    if mode == 1:
        return ("a", reg)
    if mode == 2:
        return ("ind", reg)
    if mode == 3:
        return ("post", reg)
    if mode == 4:
        return ("pre", reg)
    if mode == 5:
        return ("disp", reg, r.sword())
    if mode == 6:
        ext = r.word()
        if ext & 0x100:
            raise DecodeError("68020 full extension word")
        return ("idx", reg, _s8(ext & 0xFF), "a" if ext & 0x8000 else "d", (ext >> 12) & 7,
                4 if ext & 0x800 else 2)
    if mode == 7:
        if reg == 0:
            return ("abs", r.sword() & 0xFFFFFFFF)
        if reg == 1:
            return ("abs", r.long())
        if reg == 2:
            base = r.pc
            return ("pcdisp", (base + r.sword()) & 0xFFFFFFFF)
        if reg == 3:
            base = r.pc
            ext = r.word()
            return ("pcidx", (base + _s8(ext & 0xFF)) & 0xFFFFFFFF,
                    "a" if ext & 0x8000 else "d", (ext >> 12) & 7, 4 if ext & 0x800 else 2)
        if reg == 4:
            if size == 4:
                return ("imm", r.long())
            w = r.word()
            return ("imm", w & 0xFF if size == 1 else w)
    raise DecodeError(f"addressing mode {mode}/{reg}")


def decode(image, address):
    w = image.word(address)
    r = _Reader(image, address)
    i = Insn(address)
    hi4 = w >> 12
    mode, reg = (w >> 3) & 7, w & 7
    opmode = (w >> 6) & 7
    rn = (w >> 9) & 7

    if hi4 == 0xF:  # line F: the X68000's system calls
        i.op = "linef"
        i.extra = w
    elif hi4 in (1, 2, 3):  # MOVE / MOVEA
        size = {1: 1, 3: 2, 2: 4}[hi4]
        i.size = size
        i.src = _ea(r, mode, reg, size)
        dmode = (w >> 6) & 7
        i.dst = _ea(r, dmode, rn, size)
        i.op = "movea" if dmode == 1 else "move"
    elif hi4 == 0x7:
        if w & 0x100:
            raise DecodeError("line 7 with bit 8")
        i.op, i.size, i.src, i.dst = "moveq", 4, ("imm", _s8(w & 0xFF) & 0xFFFFFFFF), ("d", rn)
    elif hi4 == 0x6:  # Bcc / BRA / BSR
        cond = (w >> 8) & 15
        disp = _s8(w & 0xFF)
        if disp == 0:
            disp = r.sword()
        elif disp == -1:
            raise DecodeError("32-bit branch")
        i.target = (address + 2 + disp) & 0xFFFFFFFF
        if cond == 0:
            i.op, i.flow = "bra", "jump"
        elif cond == 1:
            i.op, i.flow = "bsr", "call"
        else:
            i.op, i.cond, i.flow = "bcc", CONDITIONS[cond], "branch"
    elif hi4 == 0x5:
        size_bits = (w >> 6) & 3
        if size_bits == 3:
            cond = CONDITIONS[(w >> 8) & 15]
            if mode == 1:  # DBcc
                i.op, i.cond, i.dst = "dbcc", cond, ("d", reg)
                i.target = (address + 2 + r.sword()) & 0xFFFFFFFF
                i.flow = "branch"
            else:
                i.op, i.cond, i.size, i.dst = "scc", cond, 1, _ea(r, mode, reg, 1)
        else:
            i.size = SIZES[size_bits]
            i.op = "subq" if w & 0x100 else "addq"
            i.src, i.dst = ("imm", rn or 8), _ea(r, mode, reg, i.size)
    elif hi4 == 0x4:
        _line4(r, i, w, mode, reg, rn)
    elif hi4 in (0x8, 0x9, 0xB, 0xC, 0xD):
        _arith(r, i, w, hi4, mode, reg, opmode, rn)
    elif hi4 == 0xE:
        _shift(r, i, w, mode, reg, rn)
    elif hi4 == 0x0:
        _line0(r, i, w, mode, reg, rn)
    else:
        raise DecodeError(f"line {hi4:X}")
    i.length = r.pc - address
    return i


def _line0(r, i, w, mode, reg, rn):
    if w & 0x100:  # dynamic bit operations
        if mode == 1:
            raise DecodeError("MOVEP")
        i.op = ("btst", "bchg", "bclr", "bset")[(w >> 6) & 3]
        i.src = ("d", rn)
        i.size = 4 if mode == 0 else 1
        i.dst = _ea(r, mode, reg, i.size)
        return
    top = (w >> 9) & 7
    if top == 4:  # static bit operations
        bit = r.word() & 0xFF
        i.op = ("btst", "bchg", "bclr", "bset")[(w >> 6) & 3]
        i.src = ("imm", bit)
        i.size = 4 if mode == 0 else 1
        i.dst = _ea(r, mode, reg, i.size)
        return
    names = {0: "ori", 1: "andi", 2: "subi", 3: "addi", 5: "eori", 6: "cmpi"}
    if top not in names:
        raise DecodeError(f"line 0 operation {top}")
    size_bits = (w >> 6) & 3
    if size_bits == 3:
        raise DecodeError("line 0 size 3")
    size = SIZES[size_bits]
    if mode == 7 and reg == 4:  # to CCR / SR
        value = r.word()
        i.op, i.size = names[top], size
        i.src = ("imm", value & (0xFF if size == 1 else 0xFFFF))
        i.dst = ("ccr",) if size == 1 else ("sr",)
        return
    i.op, i.size = names[top], size
    i.src = ("imm", r.long() if size == 4 else (r.word() & (0xFF if size == 1 else 0xFFFF)))
    i.dst = _ea(r, mode, reg, size)


def _line4(r, i, w, mode, reg, rn):
    if w == 0x4E75:
        i.op, i.flow = "rts", "return"
        return
    if w == 0x4E73:
        i.op, i.flow = "rte", "rte"
        return
    if w == 0x4E71:
        i.op = "nop"
        return
    if w == 0x4E72:
        i.op, i.src = "stop", ("imm", r.word())
        return
    if (w & 0xFFF0) == 0x4E40:
        i.op, i.extra = "trap", w & 15
        return
    if (w & 0xFFF8) == 0x4E50:
        i.op, i.dst, i.src = "link", ("a", reg), ("imm", r.sword())
        return
    if (w & 0xFFF8) == 0x4E58:
        i.op, i.dst = "unlk", ("a", reg)
        return
    if (w & 0xFFF0) == 0x4E60:
        i.op = "move_usp"
        i.src, i.dst = (("a", reg), ("usp",)) if not w & 8 else (("usp",), ("a", reg))
        return
    if (w & 0xFFC0) == 0x4E80:
        i.op, i.flow, i.dst = "jsr", "call", _ea(r, mode, reg, 4)
        if i.dst[0] in ("abs", "pcdisp"):
            i.target = i.dst[1]
        return
    if (w & 0xFFC0) == 0x4EC0:
        i.op, i.flow, i.dst = "jmp", "jump", _ea(r, mode, reg, 4)
        if i.dst[0] in ("abs", "pcdisp"):
            i.target = i.dst[1]
        return
    if (w & 0xF1C0) == 0x41C0:
        i.op, i.size, i.src, i.dst = "lea", 4, _ea(r, mode, reg, 4), ("a", rn)
        return
    if (w & 0xFFF8) == 0x4840:
        i.op, i.size, i.dst = "swap", 4, ("d", reg)
        return
    if (w & 0xFFC0) == 0x4840:
        i.op, i.size, i.src = "pea", 4, _ea(r, mode, reg, 4)
        return
    if (w & 0xFFB8) == 0x4880:  # EXT
        i.op, i.size, i.dst = "ext", 4 if w & 0x40 else 2, ("d", reg)
        return
    if (w & 0xFB80) == 0x4880:  # MOVEM
        size = 4 if w & 0x40 else 2
        mask = r.word()
        ea = _ea(r, mode, reg, size)
        i.op, i.size = "movem", size
        if w & 0x400:
            i.src, i.dst = ea, ("list", mask, False)
        else:
            i.src, i.dst = ("list", mask, mode == 4), ea
        return
    if (w & 0xFFC0) == 0x40C0:
        i.op, i.size, i.src, i.dst = "move", 2, ("sr",), _ea(r, mode, reg, 2)
        return
    if (w & 0xFFC0) == 0x44C0:
        i.op, i.size, i.src, i.dst = "move", 2, _ea(r, mode, reg, 2), ("ccr",)
        return
    if (w & 0xFFC0) == 0x46C0:
        i.op, i.size, i.src, i.dst = "move", 2, _ea(r, mode, reg, 2), ("sr",)
        return
    size_bits = (w >> 6) & 3
    groups = {0x4000: "negx", 0x4200: "clr", 0x4400: "neg", 0x4600: "not", 0x4A00: "tst"}
    if (w & 0xFF00) in groups and size_bits != 3:
        i.op, i.size = groups[w & 0xFF00], SIZES[size_bits]
        i.dst = _ea(r, mode, reg, i.size)
        return
    if (w & 0xFFC0) == 0x4AC0:
        i.op, i.size, i.dst = "tas", 1, _ea(r, mode, reg, 1)
        return
    if (w & 0xF1C0) == 0x4180:
        i.op, i.size, i.src, i.dst = "chk", 2, _ea(r, mode, reg, 2), ("d", rn)
        return
    raise DecodeError(f"line 4 {w:04X}")


def _arith(r, i, w, hi4, mode, reg, opmode, rn):
    if hi4 in (0x8, 0xC) and (w & 0x1F0) == 0x100:
        raise DecodeError("SBCD/ABCD")
    if hi4 in (0x8, 0xC) and opmode in (3, 7):
        name = {(0x8, 3): "divu", (0x8, 7): "divs", (0xC, 3): "mulu", (0xC, 7): "muls"}[(hi4, opmode)]
        i.op, i.size, i.src, i.dst = name, 2, _ea(r, mode, reg, 2), ("d", rn)
        return
    if hi4 == 0xC and (w & 0x130) == 0x100:  # EXG
        kind = (w >> 3) & 0x1F
        if kind == 8:
            i.src, i.dst = ("d", rn), ("d", reg)
        elif kind == 9:
            i.src, i.dst = ("a", rn), ("a", reg)
        elif kind == 0x11:
            i.src, i.dst = ("d", rn), ("a", reg)
        else:
            raise DecodeError("EXG")
        i.op, i.size = "exg", 4
        return
    if hi4 in (0x9, 0xD) and opmode in (3, 7):
        i.op = "suba" if hi4 == 0x9 else "adda"
        i.size = 4 if opmode == 7 else 2
        i.src, i.dst = _ea(r, mode, reg, i.size), ("a", rn)
        return
    if hi4 == 0xB and opmode in (3, 7):
        i.op, i.size = "cmpa", 4 if opmode == 7 else 2
        i.src, i.dst = _ea(r, mode, reg, i.size), ("a", rn)
        return
    size = SIZES[opmode & 3]
    if hi4 in (0x9, 0xD) and opmode >= 4 and mode in (0, 1):
        i.op = "subx" if hi4 == 0x9 else "addx"
        i.size = size
        i.src, i.dst = (("d", reg), ("d", rn)) if mode == 0 else (("pre", reg), ("pre", rn))
        return
    if hi4 == 0xB and opmode >= 4 and mode == 1:
        i.op, i.size, i.src, i.dst = "cmpm", size, ("post", reg), ("post", rn)
        return
    names = {0x8: "or", 0x9: "sub", 0xB: "cmp" if opmode < 4 else "eor", 0xC: "and", 0xD: "add"}
    i.op, i.size = names[hi4], size
    ea = _ea(r, mode, reg, size)
    if opmode < 4:
        i.src, i.dst = ea, ("d", rn)
    else:
        i.src, i.dst = ("d", rn), ea


def _shift(r, i, w, mode, reg, rn):
    kinds = ("as", "ls", "rox", "ro")
    if ((w >> 6) & 3) == 3:  # memory, by one
        i.op = kinds[(w >> 9) & 3] + ("l" if w & 0x100 else "r")
        i.size = 2
        i.src = ("imm", 1)
        i.dst = _ea(r, mode, reg, 2)
        return
    i.op = kinds[(w >> 3) & 3] + ("l" if w & 0x100 else "r")
    i.size = SIZES[(w >> 6) & 3]
    i.src = ("d", rn) if w & 0x20 else ("imm", rn or 8)
    i.dst = ("d", reg)
