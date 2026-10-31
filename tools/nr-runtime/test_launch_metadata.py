"""CPU-only launch record format regression checks. Run with python -I."""
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parent))
import launch_metadata as launch


class LaunchMetadataTests(unittest.TestCase):
    native = {4: ['.entry target(']}
    pack = b'TRPNRK1\0' + struct.pack('<I', 1) + b'unchanged table and payload'

    def test_multiplier_round_trip(self):
        rule, = launch.parse(['target:32x4x1:*1x1x2:4096'], self.native)
        self.assertEqual(rule['gridMultiplier'], [1, 1, 2])
        self.assertNotIn('grid', rule)
        data = launch.append(self.pack, [rule])
        self.assertEqual(data[:8] + struct.pack('<I', 1) + data[12:len(self.pack)], self.pack)
        suffix = data[len(self.pack):]
        self.assertEqual(suffix[:16], b'TRPLCH1\0' + struct.pack('<II', 1, 1))
        self.assertEqual(struct.unpack_from('<11I', suffix, 16), (4, 3, 6, 6, 32, 4, 1, 1, 1, 2, 4096))
        self.assertEqual(suffix[60:], b'target\0\0')

    def test_legacy_bytes(self):
        self.assertIs(launch.append(self.pack, []), self.pack)
        for spec, flags, grid, shared in [('target:128x1x1', 0, (0,0,0), 0),
                ('target:128x1x1:7x2x1:0', 3, (7,2,1), 0), ('target:128x1x1::4096', 2, (0,0,0), 4096)]:
            rules = launch.parse([spec], self.native)
            expected = (self.pack[:8] + struct.pack('<I', 2) + self.pack[12:] + b'TRPLCH1\0' +
                        struct.pack('<II11I', 1, 1, 4, 3, 6, flags, 128, 1, 1, *grid, shared) + b'target\0\0')
            self.assertEqual(launch.append(self.pack, rules), expected)

    def test_invalid_multiplier(self):
        for grid in ['*0x1x1', '*1x65x1', '*1x1x0', '*-1x1x1', '*1x1', '*1x1x2junk', '**1x1x2']:
            with self.subTest(grid=grid), self.assertRaises(RuntimeError):
                launch.parse(['target:32x4x1:' + grid], self.native)
        rule, = launch.parse(['target:32x4x1:*64x64x64'], self.native)
        self.assertEqual(rule['gridMultiplier'], [64,64,64])


if __name__ == '__main__':
    unittest.main()
