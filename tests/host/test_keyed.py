"""keyed.py's ChaCha20 against OpenSSL's (when the openssl command has it),
its file format, and its round trip."""

import os
import shutil
import struct
import subprocess
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "xlat"))
import keyed  # noqa: E402


class KeyedTest(unittest.TestCase):
    def test_rfc_zero_vector(self):
        stream = keyed.chacha20(bytes(32), bytes(12), bytes(32))
        self.assertEqual(stream.hex(), "76b8e0ada0f13d90405d6ae55386bd28"
                                       "bdd219b8a08ded1aa836efcc8b770dc7")

    @unittest.skipUnless(shutil.which("openssl"), "no openssl")
    def test_against_openssl(self):
        key, nonce = bytes(range(32)), bytes(range(100, 112))
        data = bytes((i * 31 + 7) & 255 for i in range(1000))
        out = subprocess.run(["openssl", "enc", "-chacha20", "-K", key.hex(),
                              "-iv", (bytes(4) + nonce).hex()], input=data, capture_output=True)
        if out.returncode != 0:
            self.skipTest("openssl without chacha20")
        self.assertEqual(keyed.chacha20(key, nonce, data), out.stdout)

    def test_wrap(self):
        plain = bytearray(300)
        plain[keyed.IMAGE_FIRMWARE:keyed.IMAGE_FIRMWARE + 9] = b"abc1234+x"
        key = keyed.key_from(b"0000000: 4855\n")
        k = keyed.wrap("COMMON.IMG", bytes(plain), key)
        self.assertEqual(k[:4], b"CRSK")
        version, size, crc = struct.unpack_from("<III", k, 4)
        self.assertEqual((version, size, crc), (1, 300, zlib.crc32(bytes(plain))))
        self.assertEqual(k[16:26], b"COMMON.IMG")
        self.assertEqual(k[32:41], b"abc1234+x")
        back = keyed.chacha20(key, keyed.nonce_for("COMMON.IMG", "abc1234+x"), k[64:])
        self.assertEqual(back, bytes(plain))
        self.assertEqual(keyed.keyed_name("START.STA"), "START.STK")


if __name__ == "__main__":
    unittest.main()
