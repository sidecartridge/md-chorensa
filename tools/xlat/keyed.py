#!/usr/bin/env python3
"""The keyed set (rp/src/install.c's gate decrypts it): the store's images
and START.STA, each encrypted with a key that only the user's own copy of
the game gives, so that a release carries nothing usable without it.

The key: SHA-256 of binaries/x68000/sz2.hex as the Amiga port's repository
zip holds it (the program the images are translated from). Each file's
nonce: the first 12 bytes of SHA-256 of its name, a zero byte and the
firmware build it links against. The cipher: ChaCha20 (RFC 8439), its block
counter from 0. A keyed file: 'CRSK', the format's version, the plain
file's size and CRC-32 (u32, little endian), its name (16 bytes) and the
firmware build (32 bytes), both zero-padded, then the whole plain file
encrypted. COMMON.IMG becomes COMMON.IMK, START.STA START.STK.

Its output is the game's, encrypted: outside the repository, never
committed.

usage: keyed.py --images DIR --hex sz2.hex --out DIR
"""

import argparse
import hashlib
import os
import struct
import sys
import zlib

MAGIC = b"CRSK"
VERSION = 1
HEADER = 64
NAMES = ["COMMON.IMG", "MENUS.IMG"] + [f"STAGE{n}.IMG" for n in range(1, 8)] + ["START.STA"]
IMAGE_FIRMWARE = 20  # xl_store_header_t's firmware[]
START_FIRMWARE = 8   # xl_store_start_t's
FIRMWARE_BYTES = 32


def keyed_name(name):
    return name[:-1] + "K"


def key_from(program_hex):
    return hashlib.sha256(program_hex).digest()


def nonce_for(name, firmware):
    return hashlib.sha256(name.encode() + b"\0" + firmware.encode()).digest()[:12]


def _rol(v, n):
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF


def chacha20(key, nonce, data):
    """`data` XORed with ChaCha20's keystream (counter from 0)."""
    state = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574] + list(struct.unpack("<8I", key)) + \
        [0] + list(struct.unpack("<3I", nonce))
    out = bytearray(data)
    for start in range(0, len(out), 64):
        x = state[:]
        for _ in range(10):
            for a, b, c, d in ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
                               (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)):
                x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = _rol(x[d] ^ x[a], 16)
                x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = _rol(x[b] ^ x[c], 12)
                x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = _rol(x[d] ^ x[a], 8)
                x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = _rol(x[b] ^ x[c], 7)
        stream = struct.pack("<16I", *((x[i] + state[i]) & 0xFFFFFFFF for i in range(16)))
        chunk = out[start:start + 64]
        out[start:start + len(chunk)] = bytes(p ^ k for p, k in zip(chunk, stream))
        state[12] = (state[12] + 1) & 0xFFFFFFFF
    return bytes(out)


def firmware_of(name, plain):
    at = START_FIRMWARE if name == "START.STA" else IMAGE_FIRMWARE
    return plain[at:at + FIRMWARE_BYTES].split(b"\0", 1)[0].decode()


def wrap(name, plain, key):
    firmware = firmware_of(name, plain)
    header = MAGIC + struct.pack("<III", VERSION, len(plain), zlib.crc32(plain) & 0xFFFFFFFF) + \
        name.encode().ljust(16, b"\0") + firmware.encode().ljust(FIRMWARE_BYTES, b"\0")
    assert len(header) == HEADER
    return header + chacha20(key, nonce_for(name, firmware), plain)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--images", required=True, help="rp_store.py's output folder")
    ap.add_argument("--hex", required=True, help="the repository's binaries/x68000/sz2.hex")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    key = key_from(open(args.hex, "rb").read())
    os.makedirs(args.out, exist_ok=True)
    for name in NAMES:
        plain = open(os.path.join(args.images, name), "rb").read()
        keyed = wrap(name, plain, key)
        open(os.path.join(args.out, keyed_name(name)), "wb").write(keyed)
        print(f"{keyed_name(name)}: {len(keyed)} bytes, for {firmware_of(name, plain)}")


if __name__ == "__main__":
    sys.exit(main())
