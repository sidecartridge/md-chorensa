#!/usr/bin/env python3
"""Sixteen colours out of the ST's 512 for the original's frames.

Reads the Amiga port's screen dumps (screen_NNNNN.bin, see oracle.py): each a
256 x 256 picture of indices into the port's 256-entry palette, which holds
the colours of every layer the original draws (backgrounds, sprites, text).
It tries the ways the ST can show them in 16 colours of its 512 (3 bits a
channel), the same on every machine:

  stage   one mapping of the source entries to the 16 and one palette for a
          stretch of frames (a stage): the RP converts the assets once and
          drawing stays a byte copy. Colours the original changes on the
          fly (the cycling, a flash) stay as they were averaged.
  follow  one mapping for the stretch, chosen from how each entry's colour
          moves over it; each of the 16 ST colours then follows the source
          colours mapped to it, frame by frame: the cycling and the flashes
          become palette writes, which cost nothing (the RP publishes a
          palette with every frame).
  frame   a mapping and a palette for each frame: closest to the original,
          but the RP would remap every pixel it draws through a table that
          changes every frame.

Each plain, or with an ordered dither (4 x 4 Bayer) between two of the 16
for colours between them. The mapping is by source entry, as the RP's would
be (an entry is converted once, whatever pixel shows it); the dither's
pattern follows the screen here, where baked into the assets it would follow
each sprite. Errors are distances in the Oklab space, x 100 (about 1 is
the smallest difference seen side by side).

usage:
  quantise.py stats DUMP_DIR [--segment NAME:FIRST-LAST ...]
  quantise.py sheet DUMP_DIR --frames N,N,... --out DIR [--segment ...]
  quantise.py still DUMP_DIR --frame N --scheme SCHEME --out FILE [--segment ...]
  quantise.py motion DUMP_DIR [--segment ...]
  quantise.py clip DUMP_DIR --out DIR [--fit-dumps DIR] [--segment ...]

stats prints, per stretch, how many colours the frames use, which entries
cycle and which frames flash. sheet writes, for each frame asked, the
original and every scheme as PNGs at the ST's pixel aspect, and an
index.html beside them. still writes one frame as the RP's bench shows it:
256 x 256 bytes of ST colour numbers, then the 16 palette words (big-endian,
0RRR0GGG0BBB). A stretch (--segment) is a name and its first and last frame;
by default the whole run is one. motion measures, over consecutive dumps,
each scheme's error, the flicker it adds to the original's own colour changes
and the motion it loses (best on dumps of every frame). The per-frame scheme
starts each frame's fit from the frame before's, keeping an entry's colour
unless a change is worth it; motion also shows it without that continuity.
clip writes the dumps one after another with the original beside the stage,
follow and frame schemes, and a page that plays them at 50 frames a second
(the stage and follow schemes fitted on --fit-dumps, a longer run of the same
stretch, when given).
Nothing the tool writes may go into the
repository: it is the game's.
"""

import argparse
import glob
import math
import os
import re
import struct
import sys
from collections import Counter

import oracle

ST_LEVELS = [round(v * 255 / 7) for v in range(8)]
COLOURS = 16
BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
# The ST's PAL low-resolution pixel is about 0.92 as wide as it is tall:
# 125 ns of a 52 us line across 4/3 of the 288 lines' height.
ST_PIXEL_ASPECT = 0.125 / 52 * 4 / 3 * 288
SHEET_SCALE_X = 3
# A pair of dithered colours that far apart (Oklab x 100) costs this much per
# unit, so the dither prefers neighbours to noise.
DITHER_CONTRAST_PENALTY = 0.3
# Frames whose used colours move this much (mean Oklab x 100) from the
# frames around them are flashes.
FLASH_THRESHOLD = 12.0


# --- Colour --------------------------------------------------------------------

def _linear(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


# Colours are compared as they are seen on a screen in a room: with 1% of
# white added to each (viewing flare). Without it the Oklab space, which
# stretches the darkest colours, rates a near-black navy such as (0, 8, 16)
# as nearer to the ST's darkest red than to black, and a floor of it turns
# red-brown; on a screen those levels sit at the black level.
VIEWING_FLARE = 0.01


def oklab(rgb):
    """(L, a, b) x 100 of an 8-bit sRGB colour, with the viewing flare."""
    r, g, b = (_linear(c) + VIEWING_FLARE for c in rgb)
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l, m, s = (math.copysign(abs(v) ** (1 / 3), v) for v in (l, m, s))
    return (100 * (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s),
            100 * (1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
            100 * (0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s))


def dist2(p, q):
    return (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2


def st_rgb(st):
    """The 8-bit RGB the ST shows for a colour of three 3-bit levels."""
    return tuple(ST_LEVELS[v] for v in st)


def st_word(st):
    """The palette word: 0RRR0GGG0BBB."""
    r, g, b = st
    return (r << 8) | (g << 4) | b


ALL_ST = [(r, g, b) for r in range(8) for g in range(8) for b in range(8)]
ST_LAB = {st: oklab(st_rgb(st)) for st in ALL_ST}
_NEAREST = {}


def nearest_st(lab):
    """The ST colour closest to an Oklab colour."""
    key = tuple(round(v, 3) for v in lab)
    hit = _NEAREST.get(key)
    if hit is None:
        hit = min(ALL_ST, key=lambda st: dist2(ST_LAB[st], lab))
        _NEAREST[key] = hit
    return hit


def best_st_for(members, used=()):
    """The ST colour, not in `used`, with the smallest weighted error over
    `members` [(lab, weight)]: searched around the weighted mean."""
    total = sum(w for _, w in members) or 1.0
    mean = tuple(sum(lab[k] * w for lab, w in members) / total for k in range(3))
    start = nearest_st(mean)
    best, best_err = None, None
    candidates = {start}
    for dr in (-1, 0, 1):
        for dg in (-1, 0, 1):
            for db in (-1, 0, 1):
                c = (start[0] + dr, start[1] + dg, start[2] + db)
                if all(0 <= v < 8 for v in c):
                    candidates.add(c)
    for c in sorted(candidates):
        if c in used:
            continue
        err = sum(w * dist2(ST_LAB[c], lab) for lab, w in members)
        if best_err is None or err < best_err:
            best, best_err = c, err
    if best is None:  # every neighbour taken: the nearest free one
        best = min((c for c in ALL_ST if c not in used), key=lambda c: dist2(ST_LAB[c], mean))
    return best


# --- The dumps -----------------------------------------------------------------

class Frame:
    def __init__(self, number, palette, pixels):
        self.number = number
        self.palette = palette
        self.pixels = pixels
        self.counts = Counter(pixels)
        self.lab = [oklab(c) for c in palette]


def load(directory, wanted=None):
    frames = []
    for path in sorted(glob.glob(os.path.join(directory, "screen_*.bin"))):
        number = int(re.search(r"screen_(\d+)\.bin$", path).group(1))
        if wanted is not None and number not in wanted:
            continue
        width, lines, _, palette, pixels = oracle.read_screen(path)
        if (width, lines) != (256, 256):
            sys.exit(f"{path}: {width} x {lines}, not 256 x 256")
        frames.append(Frame(number, palette, pixels))
    if not frames:
        sys.exit(f"{directory}: no screen dumps")
    return frames


def segments_of(specs, frames):
    """[(name, first, last)] from NAME:FIRST-LAST, or the whole run."""
    if not specs:
        return [("all", frames[0].number, frames[-1].number)]
    out = []
    for spec in specs:
        m = re.fullmatch(r"([^:]+):(\d+)-(\d+)", spec)
        if not m:
            sys.exit(f"--segment {spec}: NAME:FIRST-LAST")
        out.append((m.group(1), int(m.group(2)), int(m.group(3))))
    return out


def in_segment(frames, seg):
    return [f for f in frames if seg[1] <= f.number <= seg[2]]


def segment_for(segs, number):
    for seg in segs:
        if seg[1] <= number <= seg[2]:
            return seg
    sys.exit(f"frame {number} is in no --segment")


# --- Weights and fits ---------------------------------------------------------

def weights(frames):
    """Each used entry's weight over the frames: the mean of the square root
    of its pixels a frame, so that a bullet's few pixels still count against
    a background's thousands."""
    w = Counter()
    for f in frames:
        for index, n in f.counts.items():
            w[index] += math.sqrt(n)
    return {i: v / len(frames) for i, v in w.items()}


def kmeans(points, k, dims, iterations=40, init=None):
    """Weighted k-means: points [(vector, weight, key)] -> ({key: cluster},
    [centre]). Started from `init` (k centres) when given, else
    deterministically: the heaviest point first, then the farthest weighted.
    A cluster left empty keeps its centre."""
    def d2(p, q):
        return sum((p[i] - q[i]) ** 2 for i in range(dims))

    if init is None:
        if len(points) <= k:
            return {key: n for n, (_, _, key) in enumerate(points)}, [p[0] for p in points]
        centres = [max(points, key=lambda p: p[1])[0]]
        while len(centres) < k:
            centres.append(max(points, key=lambda p: p[1] * min(d2(p[0], c) for c in centres))[0])
    else:
        centres = list(init)
    assign = {}
    for _ in range(iterations):
        new = {key: min(range(k), key=lambda c: d2(vec, centres[c])) for vec, _, key in points}
        if new == assign:
            break
        assign = new
        for c in range(k):
            members = [(vec, w) for vec, w, key in points if assign[key] == c]
            total = sum(w for _, w in members)
            if total > 0:
                centres[c] = tuple(sum(vec[i] * w for vec, w in members) / total
                                   for i in range(dims))
    return assign, centres


def colours_for(clusters, member_lab, member_w):
    """An ST colour per cluster (distinct), the heaviest clusters first."""
    order = sorted(set(clusters.values()),
                   key=lambda c: -sum(member_w[i] for i in clusters if clusters[i] == c))
    chosen, used = {}, set()
    for c in order:
        members = [(member_lab[i], member_w[i]) for i in clusters if clusters[i] == c]
        st = best_st_for(members, used)
        chosen[c] = st
        used.add(st)
    return chosen


class Scheme:
    """A mapping of source entries to ST colour numbers 0..15 and, per frame,
    the 16 ST colours; optionally a dither per entry."""

    def __init__(self, name, mapping, palette_for, dither=None):
        self.name = name
        self.mapping = mapping          # {entry: number}
        self.palette_for = palette_for  # frame -> [16 ST colours]
        self.dither = dither            # {entry: (number_a, number_b, level 0..16)}

    def number(self, entry, x, y):
        if self.dither is not None and entry in self.dither:
            a, b, level = self.dither[entry]
            return b if BAYER4[y & 3][x & 3] < level else a
        return self.mapping.get(entry, 0)


def mean_lab(frames, index, member_w=None):
    """An entry's colour over the frames, weighted by its pixels in each."""
    total, acc = 0.0, [0.0, 0.0, 0.0]
    for f in frames:
        n = f.counts.get(index, 0)
        w = math.sqrt(n) if n else 0.0
        if w:
            total += w
            for k in range(3):
                acc[k] += f.lab[index][k] * w
    if not total:
        return frames[0].lab[index]
    return tuple(v / total for v in acc)


def pad(colours):
    """Sixteen distinct ST colours: the unused numbers black, then greys."""
    out = list(colours)
    for st in [(0, 0, 0)] + [(v, v, v) for v in range(1, 8)] + ALL_ST:
        if len(out) >= COLOURS:
            break
        if st not in out:
            out.append(st)
    return out[:COLOURS]


def fit_stage(frames):
    """One mapping and one palette: k-means on each entry's mean colour."""
    w = weights(frames)
    lab = {i: mean_lab(frames, i) for i in w}
    clusters, _ = kmeans([(lab[i], w[i], i) for i in w], COLOURS, 3)
    colours = colours_for(clusters, lab, w)
    order = sorted(colours)
    numbers = {c: n for n, c in enumerate(order)}
    palette = pad([colours[c] for c in order])
    mapping = {i: numbers[clusters[i]] for i in clusters}
    return Scheme("stage", mapping, lambda f: palette), palette, lab


def sample(frames, count):
    if len(frames) <= count:
        return frames
    step = len(frames) / count
    return [frames[int(n * step)] for n in range(count)]


def fit_follow(frames):
    """One mapping, from each entry's colour over time (entries that cycle
    or flash together land together), and per frame the ST colour that best
    stands for each group's current colours, with the stretch's weights."""
    w = weights(frames)
    # The trail: frames spread over the stretch, and the frames where the
    # colours move most (a fade, a flash), so that entries that move
    # together, and only those, land together.
    moves = sorted(((sum(1 for i in w if a.palette[i] != b.palette[i]), n + 1)
                    for n, (a, b) in enumerate(zip(frames, frames[1:]))), reverse=True)
    busy = sorted(n for count, n in moves[:16] if count)
    trail = sample(frames, 32) + [frames[n] for n in busy]
    points = []
    for i in w:
        vec = tuple(v for f in trail for v in f.lab[i])
        points.append((vec, w[i], i))
    clusters, _ = kmeans(points, COLOURS, 3 * len(trail))
    groups = sorted(set(clusters.values()))
    numbers = {c: n for n, c in enumerate(groups)}
    mapping = {i: numbers[clusters[i]] for i in clusters}
    cache = {}
    heaviest_first = sorted(groups, key=lambda c: -sum(w[i] for i in clusters if clusters[i] == c))

    def palette_of(lab_of):
        used, colours = set(), []
        for c in heaviest_first:
            members = [(lab_of(i), w[i]) for i in clusters if clusters[i] == c]
            st = best_st_for(members, used)
            used.add(st)
            colours.append((numbers[c], st))
        return pad([st for _, st in sorted(colours)])

    def palette_for(frame):
        hit = cache.get(frame.number)
        if hit is None:
            hit = cache[frame.number] = palette_of(lambda i: frame.lab[i])
        return hit

    lab = {i: mean_lab(frames, i) for i in w}
    # The dither is baked into the assets once: it is chosen against the
    # stretch's average colours, not any one frame's.
    return Scheme("follow", mapping, palette_for), palette_of(lambda i: lab[i]), lab


def fit_frame(frame):
    """A mapping and a palette for one frame."""
    s, _, lab = fit_stage([frame])
    s.name = "frame"
    return s


def _srgb(v):
    return 255 * (12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055)


def mix_lab(palette, a, b, level):
    """The Oklab colour of a 4 x 4 mix of `level` sixteenths of b in a, as
    the eye sums them: in linear light."""
    la = [_linear(c) for c in st_rgb(palette[a])]
    lb = [_linear(c) for c in st_rgb(palette[b])]
    t = level / 16
    return oklab(tuple(_srgb((1 - t) * la[k] + t * lb[k]) for k in range(3)))


# The per-frame fit keeps what it had in the frame before unless a change
# is worth it: an entry keeps its ST colour number unless another colour is
# closer by FRAME_KEEP_ENTRY (Oklab x 100); a cluster keeps its ST colour
# unless another lowers its error by FRAME_KEEP_COLOUR of it.
FRAME_KEEP_ENTRY = 6.0
FRAME_KEEP_COLOUR = 0.5
# Continuity can trap the fit in a palette that no longer suits (a cluster
# of dark navies and browns kept at dark red since a fade): each frame is
# also fitted afresh, and the fresh fit is taken, its colours matched to the
# old numbers, when it cuts the frame's error by FRAME_RESET or more.
FRAME_RESET = 0.35


def fit_frames(frames):
    """A mapping and a palette for each frame of the stretch, in order, each
    started from the frame before's (a palette that jumps from one frame to
    the next shimmers): {frame number: Scheme}."""
    out = {}
    centres, prev_palette, prev_map = None, None, {}
    for f in frames:
        points = [(f.lab[i], math.sqrt(n), i) for i, n in f.counts.items()]
        if centres is None:
            first, _, _ = fit_stage([f])
            clusters = {i: first.mapping[i] for i in f.counts}
            palette = first.palette_for(f)
            centres = [ST_LAB[st] for st in palette]
            for c in range(COLOURS):
                members = [f.lab[i] for i in clusters if clusters[i] == c]
                if members:
                    centres[c] = tuple(sum(m[k] for m in members) / len(members) for k in range(3))
        else:
            clusters, centres = kmeans(points, COLOURS, 3, iterations=12, init=centres)
            weight = {i: math.sqrt(n) for i, n in f.counts.items()}
            order = sorted(range(COLOURS), key=lambda c: -sum(weight[i] for i in clusters
                                                              if clusters[i] == c))
            palette, used = list(prev_palette), set()
            for c in order:
                members = [(f.lab[i], weight[i]) for i in clusters if clusters[i] == c]
                if not members:
                    continue
                best = best_st_for(members, used)

                def err(st):
                    return sum(w * dist2(ST_LAB[st], lab) for lab, w in members)
                keep = prev_palette[c]
                if keep not in used and err(keep) <= (1 + FRAME_KEEP_COLOUR) * err(best):
                    best = keep
                palette[c] = best
                used.add(best)
            # Two clusters may now share a colour with one left empty: the
            # empty one takes the nearest free colour to its old one.
            seen = set()
            for c in range(COLOURS):
                if palette[c] in seen:
                    palette[c] = min((st for st in ALL_ST if st not in palette),
                                     key=lambda st: dist2(ST_LAB[st], ST_LAB[palette[c]]))
                seen.add(palette[c])
        mapping = _frame_mapping(f, palette, prev_map)
        if prev_palette is not None:
            fresh, _, _ = fit_stage([f])
            fresh_palette = _matched(fresh.palette_for(f), palette)
            fresh_mapping = _frame_mapping(f, fresh_palette, prev_map)
            if (_frame_error(f, fresh_palette, fresh_mapping)
                    < (1 - FRAME_RESET) * _frame_error(f, palette, mapping)):
                palette, mapping = fresh_palette, fresh_mapping
                centres = [ST_LAB[st] for st in palette]
        out[f.number] = Scheme("frame", mapping, (lambda p: lambda fr: p)(list(palette)))
        prev_palette, prev_map = palette, mapping
    return out


def _frame_mapping(f, palette, prev_map):
    """Each used entry to its nearest colour of `palette`, or to the number it
    had in the frame before when that is within FRAME_KEEP_ENTRY of it."""
    plab = [ST_LAB[st] for st in palette]
    mapping = {}
    for i in f.counts:
        best = min(range(COLOURS), key=lambda n: dist2(plab[n], f.lab[i]))
        prev = prev_map.get(i)
        if prev is not None and (dist2(plab[prev], f.lab[i]) ** 0.5
                                 <= dist2(plab[best], f.lab[i]) ** 0.5 + FRAME_KEEP_ENTRY):
            best = prev
        mapping[i] = best
    return mapping


def _frame_error(f, palette, mapping):
    return sum(n * dist2(ST_LAB[palette[mapping[i]]], f.lab[i]) ** 0.5
               for i, n in f.counts.items())


def _matched(new, old):
    """`new`'s colours renumbered to sit where `old` had the same or the
    closest colour (greedy, closest pairs first), so a fresh palette moves
    as few numbers as it can."""
    pairs = sorted((dist2(ST_LAB[a], ST_LAB[b]), i, j)
                   for i, a in enumerate(new) for j, b in enumerate(old))
    placed, taken, out = set(), set(), [None] * COLOURS
    for _, i, j in pairs:
        if i in placed or j in taken:
            continue
        out[j] = new[i]
        placed.add(i)
        taken.add(j)
    return out


def add_dither(scheme, palette, lab):
    """For each entry, the pair of the palette's colours and the share of the
    second whose 4 x 4 mix comes closest, with a penalty for pairs far apart;
    no dither where one colour is as close."""
    plab = [ST_LAB[st] for st in palette]
    mixes = []
    for a in range(COLOURS):
        for b in range(a + 1, COLOURS):
            penalty = DITHER_CONTRAST_PENALTY * dist2(plab[a], plab[b]) ** 0.5
            for level in range(1, 16):
                mixes.append((mix_lab(palette, a, b, level), penalty, a, b, level))
    dither = {}
    for entry, colour in lab.items():
        single = scheme.mapping[entry]
        best_err, best = dist2(plab[single], colour) ** 0.5, None
        for mlab, penalty, a, b, level in mixes:
            if penalty >= best_err:
                continue
            err = dist2(mlab, colour) ** 0.5 + penalty
            if err < best_err:
                best_err, best = err, (a, b, level)
        if best is not None:
            dither[entry] = best
    return Scheme(scheme.name + "+dither", scheme.mapping, scheme.palette_for, dither)


# --- Rendering and errors -------------------------------------------------------

def render(scheme, frame):
    """The frame as the ST would show it: rows of 8-bit RGB, and the ST
    colour number of every pixel."""
    palette = [st_rgb(st) for st in scheme.palette_for(frame)]
    rows, numbers = [], bytearray(256 * 256)
    for y in range(256):
        row = bytearray(256 * 3)
        base = y * 256
        for x in range(256):
            n = scheme.number(frame.pixels[base + x], x, y)
            numbers[base + x] = n
            row[3 * x:3 * x + 3] = bytes(palette[n])
        rows.append(row)
    return rows, numbers


def errors(scheme, frame):
    """Mean and 99th percentile, over the pixels, of the distance between
    the source colour and the ST colour (the dither's mix where it
    dithers), Oklab x 100."""
    palette = scheme.palette_for(frame)
    per_entry = {}
    for entry in frame.counts:
        if scheme.dither is not None and entry in scheme.dither:
            shown = mix_lab(palette, *scheme.dither[entry])
        else:
            shown = ST_LAB[palette[scheme.mapping.get(entry, 0)]]
        per_entry[entry] = dist2(shown, frame.lab[entry]) ** 0.5
    total = sum(frame.counts.values())
    mean = sum(per_entry[e] * n for e, n in frame.counts.items()) / total
    ranked = sorted(frame.counts.items(), key=lambda en: per_entry[en[0]])
    seen, p99 = 0, 0.0
    for e, n in ranked:
        seen += n
        p99 = per_entry[e]
        if seen >= 0.99 * total:
            break
    return mean, p99


def original_rows(frame):
    rows = []
    for y in range(256):
        base = y * 256
        rows.append(b"".join(bytes(frame.palette[frame.pixels[base + x]]) for x in range(256)))
    return rows


def aspect_rows(rows):
    """Rows widened SHEET_SCALE_X times and repeated for the ST's pixel
    aspect (each line SHEET_SCALE_X / ST_PIXEL_ASPECT times, rounded)."""
    out = []
    acc = 0.0
    for row in rows:
        wide = b"".join(bytes(row[3 * x:3 * x + 3]) * SHEET_SCALE_X for x in range(len(row) // 3))
        acc += SHEET_SCALE_X / ST_PIXEL_ASPECT
        while acc >= 1.0:
            out.append(wide)
            acc -= 1.0
    return out


def write_aspect_png(path, rows):
    tall = aspect_rows(rows)
    oracle.write_png(path, len(tall[0]) // 3, len(tall), tall)


# --- Commands -------------------------------------------------------------------

def cmd_stats(args):
    frames = load(args.dumps)
    for seg in segments_of(args.segment, frames):
        part = in_segment(frames, seg)
        if not part:
            continue
        print(f"{seg[0]}: frames {seg[1]}-{seg[2]}, {len(part)} dumps")
        per_frame = sorted(len({f.palette[i] for i in f.counts}) for f in part)
        st_frame = sorted(len({nearest_st(f.lab[i]) for i in f.counts}) for f in part)
        union = {f.palette[i] for f in part for i in f.counts}
        st_union = {nearest_st(f.lab[i]) for f in part for i in f.counts}

        def q(v):
            return f"min {v[0]}, median {v[len(v) // 2]}, p95 {v[int(0.95 * (len(v) - 1))]}, max {v[-1]}"
        print(f"  colours a frame: {q(per_frame)}; as ST colours: {q(st_frame)}")
        print(f"  colours in the stretch: {len(union)}; as ST colours: {len(st_union)}")
        used = {i for f in part for i in f.counts}
        moving = Counter()
        for prev, cur in zip(part, part[1:]):
            for i in used:
                if prev.palette[i] != cur.palette[i]:
                    moving[i] += 1
        steps = max(len(part) - 1, 1)
        cycling = sorted(i for i, n in moving.items() if n >= 0.25 * steps)
        changes = sorted(n for n in (sum(1 for i in used if a.palette[i] != b.palette[i])
                                     for a, b in zip(part, part[1:])))
        if changes:
            print(f"  entries in use: {len(used)}; changing between dumps: median "
                  f"{changes[len(changes) // 2]}, max {changes[-1]}; changing in a quarter of "
                  f"them or more: {len(cycling)} ({', '.join(map(str, cycling[:24]))}"
                  f"{'...' if len(cycling) > 24 else ''})")
        flashes = []
        for n, f in enumerate(part):
            if 0 < n < len(part) - 1:
                around = [part[n - 1], part[n + 1]]
                shift = sum(f.counts[i] * min(dist2(f.lab[i], g.lab[i]) ** 0.5 for g in around)
                            for i in f.counts) / 65536
                if shift > FLASH_THRESHOLD:
                    flashes.append((f.number, round(shift, 1)))
        print(f"  flashes: {len(flashes)}" + (f" (frame, shift: {flashes[:12]})" if flashes else ""))
    return 0


SCHEMES = ("stage", "stage+dither", "follow", "follow+dither", "frame", "frame+dither")


_FITS = {}


def schemes_for(frames, seg, frame):
    """Every scheme for a frame of the stretch `seg` (the stretch's fits once
    per stretch)."""
    if seg not in _FITS:
        part = in_segment(frames, seg)
        stage, stage_pal, stage_lab = fit_stage(part)
        follow, follow_pal, follow_lab = fit_follow(part)
        _FITS[seg] = {"stage": stage, "stage+dither": add_dither(stage, stage_pal, stage_lab),
                      "follow": follow,
                      "follow+dither": add_dither(follow, follow_pal, follow_lab),
                      "frames": fit_frames(part)}
    out = {k: v for k, v in _FITS[seg].items() if k != "frames"}
    per_frame = _FITS[seg]["frames"][frame.number]
    out["frame"] = per_frame
    frame_lab = {i: frame.lab[i] for i in frame.counts}
    out["frame+dither"] = add_dither(per_frame, per_frame.palette_for(frame), frame_lab)
    return out


def cmd_sheet(args):
    wanted = [int(n) for n in args.frames.split(",")]
    frames = load(args.dumps)
    by_number = {f.number: f for f in frames}
    segs = segments_of(args.segment, frames)
    os.makedirs(args.out, exist_ok=True)
    rows_html = []
    for number in wanted:
        frame = by_number.get(number)
        if frame is None:
            sys.exit(f"no dump of frame {number}")
        seg = segment_for(segs, number)
        schemes = schemes_for(frames, seg, frame)
        write_aspect_png(os.path.join(args.out, f"{number:05d}-original.png"), original_rows(frame))
        cells = [(f"{number:05d}-original.png", "original",
                  f"{len({frame.palette[i] for i in frame.counts})} colours")]
        for name in SCHEMES:
            rows, _ = render(schemes[name], frame)
            png = f"{number:05d}-{name.replace('+', '-')}.png"
            write_aspect_png(os.path.join(args.out, png), rows)
            mean, p99 = errors(schemes[name], frame)
            cells.append((png, name, f"error {mean:.1f} mean, {p99:.1f} p99"))
            print(f"frame {number} ({seg[0]}) {name:14} error {mean:5.1f} mean {p99:5.1f} p99")
        rows_html.append((number, seg[0], cells))
    with open(os.path.join(args.out, "index.html"), "w") as out:
        out.write(SHEET_HTML_HEAD)
        for number, seg_name, cells in rows_html:
            out.write(f"<h2>Frame {number} <small>{seg_name}</small></h2>\n<div class=row>\n")
            for png, name, note in cells:
                out.write(f"<figure><img src='{png}' alt='{name}'><figcaption><b>{name}</b> "
                          f"{note}</figcaption></figure>\n")
            out.write("</div>\n")
        out.write("</body></html>\n")
    print(os.path.join(args.out, "index.html"))
    return 0


SHEET_HTML_HEAD = """<!doctype html><html><head><meta charset=utf-8>
<title>Sixteen colours</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; }
@media (prefers-color-scheme: dark) { :root { --bg: #1b1b1a; --fg: #ececea; --muted: #9a9a94; } }
body { background: var(--bg); color: var(--fg); font: 14px/1.4 system-ui, sans-serif; margin: 16px; }
.row { display: flex; gap: 12px; overflow-x: auto; padding-bottom: 8px; }
figure { margin: 0; flex: 0 0 auto; }
img { width: 384px; image-rendering: pixelated; display: block; }
figcaption { color: var(--muted); font-size: 12px; margin-top: 4px; }
figcaption b { color: var(--fg); font-weight: 600; }
small { color: var(--muted); font-weight: normal; }
</style></head><body>
<h1>Sixteen colours of the ST's 512</h1>
<p>Each frame as the original shows it, then each scheme, all at the ST's pixel aspect. Errors in
Oklab x 100 over the pixels (about 1 is the smallest difference seen side by side).</p>
"""


def motion(frames, scheme_at):
    """Over consecutive frames: the error (mean over pixels), the flicker
    the scheme adds to the original's own colour changes (mean over pixels
    of how much more an entry's shown colour moves than its source colour),
    the share of pixels where that is over 5, and the motion it loses (how
    much less the shown colour moves than the source's: the cycling and
    fades a fixed palette cannot show)."""
    err_sum = add_sum = lost_sum = 0.0
    visible = pixels = 0
    for a, b in zip(frames, frames[1:]):
        sa, sb = scheme_at(a), scheme_at(b)
        pa, pb = sa.palette_for(a), sb.palette_for(b)
        for e, n in b.counts.items():
            shown_b = ST_LAB[pb[sb.mapping.get(e, 0)]]
            err_sum += n * dist2(shown_b, b.lab[e]) ** 0.5
            if e not in a.counts:
                continue
            shown_a = ST_LAB[pa[sa.mapping.get(e, 0)]]
            moved = dist2(shown_a, shown_b) ** 0.5
            source = dist2(a.lab[e], b.lab[e]) ** 0.5
            added = max(0.0, moved - source)
            add_sum += n * added
            lost_sum += n * max(0.0, source - moved)
            visible += n if added > 5 else 0
            pixels += n
    total = 65536 * (len(frames) - 1) or 1
    return err_sum / total, add_sum / (pixels or 1), visible / (pixels or 1), lost_sum / (pixels or 1)


def cmd_motion(args):
    frames = load(args.dumps)
    for seg in segments_of(args.segment, frames):
        part = in_segment(frames, seg)
        if len(part) < 2:
            continue
        stage, _, _ = fit_stage(part)
        follow, _, _ = fit_follow(part)
        coherent = fit_frames(part)
        alone = {}

        def frame_alone(f):
            if f.number not in alone:
                alone[f.number] = fit_frame(f)
            return alone[f.number]
        print(f"{seg[0]}: frames {seg[1]}-{seg[2]}, {len(part)} dumps "
              f"({part[1].number - part[0].number} frames apart)")
        print(f"  {'scheme':22}{'error':>8}{'flicker':>9}{'over 5':>8}{'lost':>7}")
        for name, at in (("stage", lambda f: stage), ("follow", lambda f: follow),
                         ("frame", lambda f: coherent[f.number]),
                         ("frame, no continuity", frame_alone)):
            e, add, vis, lost = motion(part, at)
            print(f"  {name:22}{e:8.1f}{add:9.2f}{100 * vis:7.1f}%{lost:7.2f}")
    return 0


CLIP_SCHEMES = ("stage", "follow", "frame")


def cmd_clip(args):
    """The dumps one after another, the original beside each scheme, and a
    player (index.html) that shows them at 50 frames a second."""
    frames = load(args.dumps)
    fit_frames_from = load(args.fit_dumps) if args.fit_dumps else frames
    seg = segments_of(args.segment, fit_frames_from)[0]
    part = in_segment(fit_frames_from, seg)
    stage, _, _ = fit_stage(part)
    follow, _, _ = fit_follow(part)
    coherent = fit_frames(frames)
    os.makedirs(os.path.join(args.out, "frames"), exist_ok=True)
    for f in frames:
        panels = [original_rows(f)] + [render(s, f)[0] for s in (stage, follow, coherent[f.number])]
        rows = [b"".join(bytes(p[y]) for p in panels) for y in range(256)]
        oracle.write_png(os.path.join(args.out, "frames", f"{f.number:05d}.png"),
                         256 * len(panels), 256, rows)
    names = [f"{f.number:05d}" for f in frames]
    with open(os.path.join(args.out, "index.html"), "w") as out:
        out.write(CLIP_HTML.replace("@FRAMES@", ",".join(f'"{n}"' for n in names))
                  .replace("@LABELS@", " | ".join(("original",) + CLIP_SCHEMES))
                  .replace("@ASPECT@", f"{1 / ST_PIXEL_ASPECT:.4f}"))
    print(os.path.join(args.out, "index.html"))
    return 0


CLIP_HTML = """<!doctype html><html><head><meta charset=utf-8><title>Sixteen colours in motion</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; }
@media (prefers-color-scheme: dark) { :root { --bg: #1b1b1a; --fg: #ececea; --muted: #9a9a94; } }
body { background: var(--bg); color: var(--fg); font: 14px/1.4 system-ui, sans-serif; margin: 16px; }
canvas { width: 100%; max-width: 2048px; image-rendering: pixelated; display: block; background: #000; }
button { font: inherit; margin-right: 8px; }
p { color: var(--muted); }
</style></head><body>
<h1>Sixteen colours in motion</h1>
<p>@LABELS@ &middot; at the ST's pixel aspect, 50 frames a second. Space pauses, arrows step.</p>
<p><button id=play>Pause</button><button id=slow>Slow (10 fps)</button><span id=n></span></p>
<canvas id=c width=1024 height=256></canvas>
<script>
const frames = [@FRAMES@], aspect = @ASPECT@;
const c = document.getElementById("c"), g = c.getContext("2d"), n = document.getElementById("n");
c.style.aspectRatio = (1024 / (256 * aspect)).toString();
const images = frames.map(f => { const i = new Image(); i.src = "frames/" + f + ".png"; return i; });
let at = 0, playing = true, period = 20, last = 0;
function show() { const i = images[at]; if (i.complete) { g.imageSmoothingEnabled = false; g.drawImage(i, 0, 0); } n.textContent = " frame " + frames[at]; }
function tick(t) { if (playing && t - last >= period) { last = t; at = (at + 1) % frames.length; show(); } requestAnimationFrame(tick); }
document.getElementById("play").onclick = e => { playing = !playing; e.target.textContent = playing ? "Pause" : "Play"; };
document.getElementById("slow").onclick = e => { period = period === 20 ? 100 : 20; e.target.textContent = period === 20 ? "Slow (10 fps)" : "Full speed"; };
addEventListener("keydown", e => {
  if (e.key === " ") { playing = !playing; e.preventDefault(); }
  if (e.key === "ArrowRight") { playing = false; at = (at + 1) % frames.length; show(); }
  if (e.key === "ArrowLeft") { playing = false; at = (at + frames.length - 1) % frames.length; show(); }
});
images[0].onload = show; requestAnimationFrame(tick);
</script></body></html>
"""


def cmd_still(args):
    frames = load(args.dumps)
    by_number = {f.number: f for f in frames}
    frame = by_number.get(args.frame)
    if frame is None:
        sys.exit(f"no dump of frame {args.frame}")
    seg = segment_for(segments_of(args.segment, frames), args.frame)
    scheme = schemes_for(frames, seg, frame)[args.scheme]
    _, numbers = render(scheme, frame)
    palette = scheme.palette_for(frame)
    with open(args.out, "wb") as out:
        out.write(bytes(numbers))
        out.write(b"".join(struct.pack(">H", st_word(st)) for st in palette))
    print(f"{args.out}: frame {args.frame}, {args.scheme}, palette "
          + " ".join(f"{st_word(st):03X}" for st in palette))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("stats", "sheet", "still", "motion", "clip"):
        p = sub.add_parser(name)
        p.add_argument("dumps", help="a folder of screen_NNNNN.bin")
        p.add_argument("--segment", action="append", default=[],
                       help="NAME:FIRST-LAST, a stretch of frames fitted together")
    sub.choices["sheet"].add_argument("--frames", required=True, help="N,N,...")
    sub.choices["sheet"].add_argument("--out", required=True)
    sub.choices["clip"].add_argument("--out", required=True)
    sub.choices["clip"].add_argument("--fit-dumps", help="the dumps the stage and follow schemes "
                                     "are fitted on (default: the clip's own)")
    sub.choices["still"].add_argument("--frame", type=int, required=True)
    sub.choices["still"].add_argument("--scheme", choices=SCHEMES, required=True)
    sub.choices["still"].add_argument("--out", required=True)
    args = parser.parse_args(argv)
    return {"stats": cmd_stats, "sheet": cmd_sheet, "still": cmd_still,
            "motion": cmd_motion, "clip": cmd_clip}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
