"""CPU contract checks for the prepared FFN experiment (no runtime required)."""
import math
import struct
import unittest


def decode_bits(b):
    bits = ((b & 127) << 7) | ((b & 128) << 8)
    value = struct.unpack("<e", struct.pack("<H", bits))[0] * 256
    return struct.pack("<e", value)


class PreparedWeights(unittest.TestCase):
    def test_all_byte_values_against_numeric_format(self):
        # The existing decode treats exponent 15 as finite (including 0x7f).
        # This intentionally preserves its behavior rather than changing NaNs.
        for b in range(256):
            exponent, mantissa = (b >> 3) & 15, b & 7
            value = mantissa / 512 if exponent == 0 else (1 + mantissa / 8) * 2 ** (exponent - 7)
            value = math.copysign(value, -1 if b & 128 else 1)
            self.assertEqual(decode_bits(b), struct.pack("<e", value))

    def test_four_byte_lane_order(self):
        for base in range(256):
            b = [(base + step) % 256 for step in (0, 3, 89, 177)]
            word = int.from_bytes(bytes(b), "little")
            even = ((word & 0x007F007F) << 7) | ((word & 0x00800080) << 8)
            odd = ((word & 0x7F007F00) >> 1) | (word & 0x80008000)
            actual = b"".join(struct.pack("<e", struct.unpack("<e", struct.pack("<H", (pair >> shift) & 65535))[0] * 256)
                              for pair in (even, odd) for shift in (0, 16))
            self.assertEqual(actual, b"".join(decode_bits(b[i]) for i in (0, 2, 1, 3)))

    def test_matrix_bounds(self):
        offsets = (0, 512, 1024, 1536, 131072, 131584, 132096, 132608)
        # Covers every vector load for one complete bank. z adds exactly 4 MiB.
        seen = set()
        for tile in range(32):
            for warp in range(2):  # Other two warps repeat the same weight rows.
                row = (warp << 6) | (tile << 7)
                for lane in range(32):
                    for k in range(0, 1024, 64):
                        for offset in offsets:
                            at = row * 32 + lane * 16 + k * 4096 + offset
                            seen.add(at)
        self.assertEqual(len(seen), 4 * 1024 * 1024 // 16)
        self.assertEqual(min(seen), 0)
        self.assertEqual(max(seen) + 16, 4 * 1024 * 1024)


if __name__ == "__main__":
    unittest.main()
