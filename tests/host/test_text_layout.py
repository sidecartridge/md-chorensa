"""tools/xlat/rp_image.py's layout of the text plane (xl_store.h's
XL_STORE_TEXT_*): every byte the screen shows (the first 32 of each of the
screen's lines, four planes) has a place of its own in the packed area, read
and written through its page as the RP's accessors do (the page's pointer
plus the offset in the page), every page stays inside the area, and the
snapshot's text plane lands where those pages read it."""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "xlat"))
import rp_image  # noqa: E402

TEXT_PAGES = 4 * rp_image.TEXT_PLANE // rp_image.PAGE


def shown():
    """The addresses of the bytes the screen shows."""
    for plane in range(4):
        for line in range(rp_image.TEXT_LINES):
            for column in range(rp_image.TEXT_SHOWN):
                yield (rp_image.TEXT_VRAM + plane * rp_image.TEXT_PLANE
                       + line * rp_image.TEXT_LINE + column)


def packed_at(address):
    page = address // rp_image.PAGE
    at = rp_image.text_page_at(page)
    return None if at is None else at + address % rp_image.PAGE


class TextLayout(unittest.TestCase):
    def test_every_shown_byte_has_a_place_of_its_own(self):
        places = {}
        for address in shown():
            at = packed_at(address)
            self.assertIsNotNone(at, f"{address:06X} has no place")
            self.assertNotIn(at, places, f"{address:06X} and {places.get(at, 0):06X} share {at:X}")
            places[at] = address
        self.assertEqual(len(places), 4 * rp_image.TEXT_PLANE_BYTES)

    def test_pages_stay_inside_the_area(self):
        first = rp_image.TEXT_VRAM // rp_image.PAGE
        mapped = 0
        for page in range(first, first + TEXT_PAGES):
            at = rp_image.text_page_at(page)
            if at is None:
                line = (page * rp_image.PAGE - rp_image.TEXT_VRAM) % rp_image.TEXT_PLANE // rp_image.TEXT_LINE
                self.assertGreaterEqual(line, rp_image.TEXT_LINES)
                continue
            mapped += 1
            self.assertGreaterEqual(at, 0)
            self.assertLessEqual(at + rp_image.PAGE, rp_image.TEXT_BYTES)
            self.assertEqual(at % 4, 0, "a long at an even address must stay a word access")
        self.assertEqual(mapped, 4 * rp_image.TEXT_LINES * rp_image.TEXT_LINE // rp_image.PAGE)

    def test_the_snapshot_reads_back_through_the_pages(self):
        memory = bytearray(rp_image.MEMORY)
        for address in shown():
            memory[address] = (address * 2654435761 >> 13) & 0xFF
        packed, where = rp_image.text_layout(memory)
        self.assertEqual(len(packed), rp_image.TEXT_BYTES)
        for address in shown():
            at = where[address // rp_image.PAGE] + address % rp_image.PAGE
            self.assertEqual(packed[at], memory[address], f"{address:06X}")


if __name__ == "__main__":
    unittest.main()
