#!/usr/bin/env python3
"""How a song sounds on the YM2149 of a plain ST, played as sampled sound,
against the STE's DMA chip: WAVs to listen to on the host before the ST's
code changes.

The model is the template's YM path (rp/src/audio.c, tools/ym_triples.py):
the song (signed 8-bit at 12,517 Hz, music_convert.py's .ST8) resampled
linearly to the YM's rate, each sample a set of channel volumes, the chip's
output their level as measured on an ST (the three outputs are tied
together, so they do not add: Hatari's ym2149_fixed_vol.h, --mixing), held
until the next sample. Played:

  dma       the STE: the 8-bit samples at 12,517 Hz
  ym2-RATE  two channels: the top 6 bits through the Ghostbusters table
  ym3-RATE  three channels: audio.c's table (tools/ym_triples.py)

All at 44,100 Hz, the same loudness; the error against the source (signal
to noise, dB) is printed for each.

Its output is the game's: outside the repository, never committed.

usage: ym_preview.py SONG.ST8 --mixing HATARI/src/includes/ym2149_fixed_vol.h --out DIR [--seconds 30] [--start 20] [--rates 5648,8000,10000]
"""

import argparse
import math
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import wav_to_ym4  # noqa: E402
import ym_triples  # noqa: E402

OUT_RATE = 44100


def load(path, start, seconds):
    data = open(path, "rb").read()
    rate = struct.unpack_from("<I", data, 4)[0]
    first = 32 + start * rate
    raw = data[first:first + seconds * rate]
    return rate, [b - 256 if b > 127 else b for b in raw]


def resample(samples, src, dst):
    """Linear, as rp/src/audio.c resamples."""
    out, step, pos = [], src / dst, 0.0
    while pos < len(samples) - 1:
        i = int(pos)
        f = pos - i
        out.append(samples[i] + (samples[i + 1] - samples[i]) * f)
        pos += step
    return out


def play(samples, rate, level_of):
    """The chip's output (the level held between samples) at OUT_RATE."""
    out = []
    for k in range(int(len(samples) * OUT_RATE / rate)):
        out.append(level_of(samples[int(k * rate / OUT_RATE)]))
    return out


def snr(reference, signal):
    """Against the source at the same instants, after matching the gain."""
    n = min(len(reference), len(signal))
    ref, sig = reference[:n], signal[:n]
    mr, ms = sum(ref) / n, sum(sig) / n
    ref = [v - mr for v in ref]
    sig = [v - ms for v in sig]
    gain = sum(a * b for a, b in zip(ref, sig)) / max(1e-12, sum(b * b for b in sig))
    noise = sum((a - gain * b) ** 2 for a, b in zip(ref, sig))
    power = sum(a * a for a in ref)
    return 10 * math.log10(power / max(noise, 1e-12))


def write(path, samples):
    mean = sum(samples) / len(samples)
    peak = max(abs(v - mean) for v in samples) or 1.0
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(OUT_RATE)
        w.writeframes(struct.pack(f"<{len(samples)}h",
                                  *(int(29000 * (v - mean) / peak) for v in samples)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("song")
    ap.add_argument("--mixing", required=True, help="Hatari's src/includes/ym2149_fixed_vol.h")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--start", type=int, default=20)
    ap.add_argument("--rates", default="5648,8000,10000")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rate, src = load(args.song, args.start, args.seconds)
    reference = play(src, rate, float)
    write(os.path.join(args.out, "dma.wav"), reference)
    print(f"dma: 8 bits at {rate} Hz (the reference)")
    level = ym_triples.measured_levels(args.mixing)
    two = [level(a, b, 0) for a, b in wav_to_ym4.GHOSTBUSTERS_LUT]
    _top, _triples, three = ym_triples.fit(level, ym_triples.SPAN)
    for r in (int(x) for x in args.rates.split(",")):
        at = resample(src, rate, r)
        for name, level_of in (("ym2", lambda v: two[min(63, int(v + 128) >> 2)]),
                               ("ym3", lambda v: three[min(255, int(v + 128))])):
            if name == "ym2" and r != 5648:
                continue
            out = play(at, r, level_of)
            write(os.path.join(args.out, f"{name}-{r}.wav"), out)
            print(f"{name}-{r}: {snr(reference, out):.1f} dB against the 12,517 Hz source")


if __name__ == "__main__":
    main()
