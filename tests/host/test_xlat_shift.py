"""tools/xlat's shifts by a constant count against the generic xl_shift (xl.h):
every kind but ROXL/ROXR, both directions, every size and count, on values
that cross the sign and carry edges, with X set and clear; the result and
all five flags must agree."""

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
XLAT = os.path.join(HERE, "..", "..", "tools", "xlat")
sys.path.insert(0, XLAT)
import m68k  # noqa: E402
import xlat  # noqa: E402

VALUES = [0x00000000, 0x00000001, 0x00000080, 0x0000007F, 0x000000C0, 0x00000040, 0x00008000,
          0x00007FFF, 0x0000C000, 0x00004000, 0x80000000, 0x7FFFFFFF, 0xC0000000, 0x40000000,
          0xFFFFFFFF, 0x12345678, 0x87654321, 0x0F0F0F0F, 0xF0F0F0F0, 0xA5A5A5A5]
KINDS = {0: "as", 1: "ls", 3: "ro"}  # the encoding's type field: xl_shift's kind


class Words:
    """An image of one opcode word, as m68k.decode reads it."""

    def __init__(self, word):
        self.w = word

    def word(self, address):
        return self.w


def opcode(kind, left, size, count):
    sizes = {1: 0, 2: 1, 4: 2}
    return 0xE000 | ((count & 7) << 9) | (left << 8) | (sizes[size] << 6) | (kind << 3)


class ShiftTest(unittest.TestCase):
    def test_constant_shifts_match_xl_shift(self):
        cases, body = [], []
        for kind in KINDS:
            for left in (0, 1):
                for size in (1, 2, 4):
                    for count in range(1, 9):
                        i = m68k.decode(Words(opcode(kind, left, size, count)), 0)
                        i.address = 0
                        em = xlat.Emitter(None)
                        em.emit(i, xlat.ALL_FLAGS, None)
                        n = len(cases)
                        cases.append((kind, left, size, count))
                        body.append(f"static void case_{n}(void) {{\n  {{\n"
                                    + "\n".join(em.lines) + "\n  }\n}")
        harness = ['#include <stdio.h>', '#include "xl.h"', "xl_cpu_t xl;", "uint8_t *xl_mem;",
                   *body, "typedef void (*fn)(void);",
                   "static const fn k_cases[] = {" + ",".join(f"case_{n}" for n in range(len(cases)))
                   + "};",
                   "static const int k_params[][4] = {"
                   + ",".join(f"{{{k},{l},{s},{c}}}" for k, l, s, c in cases) + "};",
                   "static const uint32_t k_values[] = {"
                   + ",".join(f"0x{v:08X}u" for v in VALUES) + "};",
                   """int main(void) {
  int bad = 0;
  for (unsigned n = 0; n < sizeof k_cases / sizeof k_cases[0]; n++) {
    int kind = k_params[n][0], left = k_params[n][1], size = k_params[n][2], count = k_params[n][3];
    uint32_t m = size == 4 ? M32 : size == 2 ? M16 : M8;
    for (unsigned k = 0; k < sizeof k_values / sizeof k_values[0]; k++) {
      for (uint32_t x = 0; x < 2; x++) {
        uint32_t v = k_values[k];
        xl.x = x; xl.n = xl.z = xl.v = xl.c = 7;
        uint32_t want = xl_shift(kind, left, v & m, (uint32_t)count, size * 8);
        uint32_t wf[5] = {xl.n, xl.z, xl.v, xl.c, xl.x};
        uint32_t want_d = (v & ~m) | want;
        xl.x = x; xl.n = xl.z = xl.v = xl.c = 7;
        D(0) = v;
        k_cases[n]();
        uint32_t gf[5] = {xl.n, xl.z, xl.v, xl.c, xl.x};
        int same = D(0) == want_d;
        for (int f = 0; f < 5; f++) same = same && (wf[f] != 0) == (gf[f] != 0);
        if (!same) {
          if (bad++ < 10) {
            printf("kind %d left %d size %d count %d v %08X x %u: D0 %08X want %08X, "
                   "nzvcx %u%u%u%u%u want %u%u%u%u%u\\n", kind, left, size, count, v, x, D(0),
                   want_d, gf[0], gf[1], gf[2], gf[3], gf[4], wf[0], wf[1], wf[2], wf[3], wf[4]);
          }
        }
      }
    }
  }
  printf("%d differ\\n", bad);
  return bad != 0;
}"""]
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = os.path.join(tmp, "shift.c"), os.path.join(tmp, "shift")
            with open(src, "w") as f:
                f.write("\n".join(harness) + "\n")
            subprocess.run(["cc", "-std=c11", "-O1", "-w", "-I", XLAT, src, "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout)


if __name__ == "__main__":
    unittest.main()
