#!/usr/bin/env python3
"""The music of a host run as the ST would play it: the game's MCDRV calls
(music.txt, run_host.py with XL_MUSIC=1) played from the converted songs
(music_convert.py's NAME.ST8) at one game frame a VBL (50 a second), into a
WAV to listen to. A song plays its intro and then its loop until it stops
or fades: MCDRV's fade, as the Amiga port measured it, lasts speed x the
song's unit / 10,000 s, down 64 steps of its table, then the song stops.
The songs keep their levels (1-64).

With --effects (the host's effects.bin, the decoded samples), the effects
of the run's effects.txt are mixed in as the RP mixes them: one at a time,
a start replacing the one playing, a stop ending it, each resampled from
its ADPCM rate to the song's, the sum saturated to 8 bits.

Its output is the game's: outside the repository, never committed.

usage: soundtrack.py RUN/music.txt --songs DIR --out FILE.wav [--first F] [--last F]
                     [--effects RUN/effects.bin] [--ext ST8]
"""

import argparse
import os
import struct
import sys
import wave

FPS = 50
# The MSM6258's rates (_ADPCMOUT's mode >> 8): 4 or 8 MHz over 1,024, 768, 512.
ADPCM_RATES = [4e6 / 1024, 4e6 / 768, 8e6 / 1024, 8e6 / 768, 8e6 / 512]
# MCDRV's fade (the Amiga port's table): the volume, of 64, at each step.
FADE_VOLUMES = [64, 59, 54, 49, 45, 42, 38, 35, 32, 29, 27, 25, 23, 21, 19, 18,
                16, 15, 14, 12, 11, 10, 10, 9, 8, 7, 7, 6, 6, 5, 5, 4,
                4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]


def load_song(path):
    data = open(path, "rb").read()
    if data[:4] != b"CRS8":
        sys.exit(f"{path}: not a song")
    rate, intro, loop, fade_unit, level = struct.unpack_from("<IIIHH", data, 4)
    samples = struct.unpack(f"{intro + loop}b", data[32:32 + intro + loop])
    return {"rate": rate, "intro": intro, "loop": loop, "fade_unit": fade_unit,
            "level": level, "samples": samples}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--songs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, default=12000)
    ap.add_argument("--effects", help="the host's effects.bin")
    ap.add_argument("--ext", default="ST8", help="the songs' files: ST8 (DMA) or YM8")
    args = ap.parse_args()
    bank, base, effect_events = None, 0, []
    if args.effects:
        raw = open(args.effects, "rb").read()
        if raw[:4] != b"CRSE":
            sys.exit(f"{args.effects}: not an effects bank")
        base, size = struct.unpack_from("<II", raw, 4)
        bank = struct.unpack(f"{size}b", raw[16:16 + size])
        for line in open(os.path.join(os.path.dirname(args.log), "effects.txt")):
            if line.startswith("#"):
                continue
            p = line.split()
            if p[1] == "out":
                effect_events.append((int(p[0]), int(p[2], 16) - base, int(p[3]), int(p[4], 16)))
            else:
                effect_events.append((int(p[0]), None, 0, 0))
    events = []
    for line in open(args.log):
        if line.startswith("#"):
            continue
        p = line.split()  # frame call [song] argument (no song before the first transfer)
        name = os.path.splitext(os.path.basename(p[2]))[0].upper() if len(p) > 3 else ""
        events.append((int(p[0]), p[1], name, int(p[-1].split("(")[0]) if p[-1][0].isdigit() else 0))
    cache = {}
    rate = None
    out = []
    song = None
    pos = 0
    fade = None  # (start sample, length in samples)
    transferred = None
    k = 0
    e = 0
    effect = None  # [position in the bank (float), end, step]
    for frame in range(args.first, args.last):
        while e < len(effect_events) and effect_events[e][0] <= frame:
            _f, at, length, mode = effect_events[e]
            e += 1
            if at is None:
                effect = None
            elif (mode & 3) and length:
                rate_out = rate or 12517
                effect = [float(at), at + length, ADPCM_RATES[min(4, mode >> 8)] / rate_out]
        while k < len(events) and events[k][0] <= frame:
            _f, call, name, argument = events[k]
            k += 1
            if call == "transfer":
                transferred = name
            elif call == "play" and transferred:
                if transferred not in cache:
                    cache[transferred] = load_song(os.path.join(args.songs, f"{transferred}.{args.ext}"))
                song, pos, fade = cache[transferred], 0, None
                rate = rate or song["rate"]
            elif call == "stop":
                song = None
            elif call == "fade" and song:
                seconds = argument * song["fade_unit"] / 10000
                fade = (len(out), max(1, round(seconds * song["rate"])))
        rate = rate or 12517
        for _ in range(round(rate * (frame + 1) / FPS) - round(rate * frame / FPS)):
            mixed = 0
            if effect:
                at = int(effect[0])
                mixed = bank[at]
                effect[0] += effect[2]
                if int(effect[0]) >= effect[1]:
                    effect = None
            if song is None:
                out.append(max(-128, min(127, mixed)))
                continue
            if fade:
                step = (len(out) - fade[0]) * 64 // fade[1]
                if step >= 64:
                    song = None
                    out.append(max(-128, min(127, mixed)))
                    continue
                volume = FADE_VOLUMES[step]
            else:
                volume = 64
            s = song["samples"][pos]
            out.append(max(-128, min(127, s * volume * song["level"] // (64 * 64) + mixed)))
            pos += 1
            if pos >= song["intro"] + song["loop"]:
                pos = song["intro"] if song["loop"] else pos - 1
                if not song["loop"]:
                    song = None
    with wave.open(args.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(out)}h", *(max(-32768, min(32767, v * 256)) for v in out)))
    print(f"{args.out}: {len(out) / rate:.0f} s at {rate} Hz, frames {args.first}-{args.last}")


if __name__ == "__main__":
    main()
