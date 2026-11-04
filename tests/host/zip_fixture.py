#!/usr/bin/env python3
"""The zip test_zipread.c reads (build/zip_fixture.zip), and each member's
content beside it (build/zip_fixture/NAME, '/' as '_'): members under a top
folder as GitHub's archives have them, deflated and stored, one larger than
inflate's 32 KB window, an empty one, and a comment after the directory."""

import os
import random
import sys
import zipfile

out = sys.argv[1] if len(sys.argv) > 1 else "build"
rng = random.Random(1995)
members = {
    "binaries/x68000/sz2.hex": b"".join(b"%07x0: %s\n" % (n, bytes(rng.choice(b"0123456789abcdef ")
                                        for _ in range(40))) for n in range(3000)),
    "binaries/amiga/MUSIC_DAT/SONG.crm": bytes(rng.randrange(256) // 4 * 4 for _ in range(150000)),
    "sources/sincos.dat": bytes(range(256)) * 4,
    "empty.txt": b"",
}
stored = {"sources/sincos.dat", "empty.txt"}
os.makedirs(os.path.join(out, "zip_fixture"), exist_ok=True)
with zipfile.ZipFile(os.path.join(out, "zip_fixture.zip"), "w") as z:
    z.writestr("repo-1234/", b"")
    for name, data in members.items():
        method = zipfile.ZIP_STORED if name in stored else zipfile.ZIP_DEFLATED
        z.writestr(zipfile.ZipInfo("repo-1234/" + name, (2026, 1, 1, 0, 0, 0)), data, method)
        with open(os.path.join(out, "zip_fixture", name.replace("/", "_")), "wb") as f:
            f.write(data)
    z.comment = b"1234567890abcdef1234567890abcdef12345678"
