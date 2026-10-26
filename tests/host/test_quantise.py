"""tools/re/quantise.py, the 16-colour schemes: the ST's palette words and
levels, colours the ST has stay exact, a fit gives 16 distinct ST colours
and keeps a picture of 16 colours exact, the dither mixes two colours for one
between them, and the per-frame fit keeps an unchanged picture unchanged
(no flicker from one frame to the next)."""

import importlib.util
import os
import sys
import unittest

from test_layout import REPO

TOOLS = os.path.join(REPO, "tools", "re")
sys.path.insert(0, TOOLS)
_spec = importlib.util.spec_from_file_location("quantise", os.path.join(TOOLS, "quantise.py"))
q = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(q)


def frame(number, palette, pixels):
    """A screen dump in memory: a 256-entry palette and 256 x 256 indices."""
    full = list(palette) + [(0, 0, 0)] * (256 - len(palette))
    return q.Frame(number, full, bytes(pixels))


def stripes(entries):
    """Horizontal bands, one per entry, each 256 / len(entries) lines."""
    band = 256 // len(entries)
    return [entries[min(y // band, len(entries) - 1)] for y in range(256) for _ in range(256)]


class Quantise(unittest.TestCase):
    def test_st_levels_and_words(self):
        self.assertEqual(q.ST_LEVELS[0], 0)
        self.assertEqual(q.ST_LEVELS[7], 255)
        self.assertEqual(q.st_word((7, 3, 1)), 0x731)
        self.assertEqual(q.st_rgb((7, 0, 7)), (255, 0, 255))
        self.assertEqual(len(q.ALL_ST), 512)

    def test_st_colours_map_to_themselves(self):
        for st in ((0, 0, 0), (7, 7, 7), (1, 0, 0), (0, 0, 1), (3, 5, 2)):
            self.assertEqual(q.nearest_st(q.oklab(q.st_rgb(st))), st)

    def test_sixteen_st_colours_stay_exact(self):
        colours = [q.st_rgb((r, g, b)) for r, g, b in
                   [(0, 0, 0), (7, 7, 7), (7, 0, 0), (0, 7, 0), (0, 0, 7), (7, 7, 0),
                    (0, 7, 7), (7, 0, 7), (3, 3, 3), (5, 2, 0), (1, 1, 4), (2, 6, 1),
                    (6, 4, 4), (4, 0, 6), (1, 3, 1), (6, 6, 2)]]
        f = frame(1, colours, stripes(list(range(16))))
        scheme, palette, _ = q.fit_stage([f])
        self.assertEqual(len(set(palette)), 16)
        mean, p99 = q.errors(scheme, f)
        self.assertAlmostEqual(mean, 0.0, places=6)
        self.assertAlmostEqual(p99, 0.0, places=6)

    def test_fit_gives_sixteen_distinct_colours(self):
        colours = [(i * 7 % 256, i * 13 % 256, i * 29 % 256) for i in range(60)]
        f = frame(1, colours, stripes(list(range(60))))
        scheme, palette, _ = q.fit_stage([f])
        self.assertEqual(len(palette), 16)
        self.assertEqual(len(set(palette)), 16)
        self.assertTrue(all(0 <= n < 16 for n in scheme.mapping.values()))

    def test_dither_mixes_for_a_colour_between_two(self):
        # Black and white, and a grey the ST does not have between two of its
        # levels only through a mix: the dither mixes black and white.
        black, white = q.st_rgb((0, 0, 0)), q.st_rgb((7, 7, 7))
        grey = (188, 188, 188)
        f = frame(1, [black, white, grey], stripes([0, 1, 2]))
        stage = q.Scheme("stage", {0: 0, 1: 1, 2: 1}, lambda fr: q.pad([(0, 0, 0), (7, 7, 7)]))
        palette = stage.palette_for(f)
        lab = {0: f.lab[0], 1: f.lab[1], 2: f.lab[2]}
        d = q.add_dither(stage, palette, lab)
        self.assertIn(2, d.dither)
        self.assertNotIn(0, d.dither)
        self.assertNotIn(1, d.dither)
        plain, _ = q.errors(stage, f)
        mixed, _ = q.errors(d, f)
        self.assertLess(mixed, plain)
        # The Bayer pattern draws the mix: both colours appear in a 4 x 4 cell.
        a, b, level = d.dither[2]
        cell = {d.number(2, x, y) for x in range(4) for y in range(4)}
        self.assertEqual(cell, {a, b})
        self.assertEqual(sum(d.number(2, x, y) == b for x in range(4) for y in range(4)), level)

    def test_frame_fit_is_steady_on_a_still_picture(self):
        colours = [(i * 37 % 256, i * 11 % 256, i * 59 % 256) for i in range(40)]
        pixels = stripes(list(range(40)))
        frames = [frame(n, colours, pixels) for n in range(1, 9)]
        fits = q.fit_frames(frames)
        error, flicker, visible, lost = q.motion(frames, lambda f: fits[f.number])
        self.assertEqual(flicker, 0.0)
        self.assertEqual(visible, 0.0)
        self.assertEqual(lost, 0.0)
        first = fits[1]
        for n in range(2, 9):
            self.assertEqual(fits[n].mapping, first.mapping)
            self.assertEqual(fits[n].palette_for(frames[n - 1]), first.palette_for(frames[0]))


if __name__ == "__main__":
    unittest.main()
