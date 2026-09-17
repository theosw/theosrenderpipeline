"""Synthetic fixtures only; no game files or downloaded tables are embedded."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("audit_address_library",
    Path(__file__).resolve().parents[1] / "tools" / "audit_address_library.py")
audit = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = audit
SPEC.loader.exec_module(audit)


def header(format_version, count, version=(1, 7, 104, 0), pointer_size=8):
    name = b"SkyrimSE.exe"
    prefix = struct.pack("<i4I", format_version, *version)
    if format_version == 5:
        return prefix + name.ljust(64, b"\0") + struct.pack("<iii", pointer_size, 0, count)
    return prefix + struct.pack("<i", len(name)) + name + struct.pack("<ii", pointer_size, count)


def sparse(format_version=2, entries=((3, 0x1200), (7, 0x3400))):
    return header(format_version, len(entries)) + b"".join(
        b"\0" + struct.pack("<QQ", identifier, offset) for identifier, offset in entries)


def dense(entries=(0, 0x1234, 0, 0xFFFFFFFC)):
    return header(5, len(entries)) + struct.pack(f"<{len(entries)}I", *entries)


class AddressLibraryTests(unittest.TestCase):
    def test_sparse_formats_preserve_ids_and_missing_gaps(self):
        for format_version in (1, 2):
            with self.subTest(format=format_version):
                library = audit.parse_library(sparse(format_version))
                self.assertEqual(library.rva(3), 0x1200)
                self.assertEqual(library.rva(7), 0x3400)
                self.assertIsNone(library.rva(4))
                self.assertIsNone(library.rva(8))

    def test_dense_header_and_direct_indexing(self):
        library = audit.parse_library(dense())
        self.assertEqual(library.version, (1, 7, 104, 0))
        self.assertEqual(library.image, "SkyrimSE.exe")
        self.assertEqual(library.entry_count, 4)
        self.assertEqual(library.data_format, 0)
        self.assertEqual(library.rva(1), 0x1234)
        self.assertEqual(library.rva(3), 0xFFFFFFFC)
        for identifier in (0, 2, 4, 0xFFFFFFFFFFFFFFFF):
            self.assertIsNone(library.rva(identifier))

    def test_every_compression_kind_and_pointer_scaling(self):
        # Seed ID=1000/RVA=8000, then independently exercise every encoded
        # value kind. Expected values are hand-calculated from that seed.
        cases = (
            (0, struct.pack("<Q", 3000), 3000),
            (1, b"", 1001),
            (2, b"\x07", 1007),
            (3, b"\x07", 993),
            (4, struct.pack("<H", 500), 1500),
            (5, struct.pack("<H", 500), 500),
            (6, struct.pack("<H", 200), 200),
            (7, struct.pack("<I", 70000), 70000),
        )
        seed = b"\0" + struct.pack("<QQ", 1000, 8000)
        for kind, payload, expected in cases:
            with self.subTest(kind=kind):
                # ID decoding with an absolute RVA.
                data = header(2, 2) + seed + bytes([kind]) + payload + struct.pack("<Q", 9000)
                self.assertEqual(audit.parse_library(data).rva(expected), 9000)
                # Scaled RVA decoding uses previous RVA / pointer size = 1000.
                data = header(2, 2) + seed + bytes([((kind | 8) << 4) | 1]) + payload
                self.assertEqual(audit.parse_library(data).rva(1001), expected * 8)

    def test_unscaled_relative_offsets(self):
        seed = b"\0" + struct.pack("<QQ", 1000, 8000)
        self.assertEqual(audit.parse_library(header(1, 2) + seed + b"\x31\x05").rva(1001), 7995)

    def test_every_truncation_is_rejected(self):
        for data in (sparse(1), sparse(2), dense()):
            for length in range(len(data)):
                with self.subTest(format=data[0], length=length):
                    with self.assertRaises(audit.InvalidLibrary):
                        audit.parse_library(data[:length])

    def test_invalid_header_fields(self):
        for data in (
            struct.pack("<i", 3),
            header(5, -1), header(5, 0x7FFFFFFF),
            header(2, -1), header(2, 0x7FFFFFFF),
            header(5, 0, pointer_size=4),
            header(2, 0, version=(1, 7, 65536, 0)),
            struct.pack("<i4Ii", 2, 1, 6, 1170, 0, -1),
        ):
            with self.subTest(data=data):
                with self.assertRaises(audit.InvalidLibrary):
                    audit.parse_library(data)

    def test_corrupt_records_and_trailing_data(self):
        for data in (
            sparse(entries=((3, 0x1200), (3, 0x1400))),
            header(2, 1) + b"\x08",
            header(2, 1) + b"\x03\x01" + struct.pack("<Q", 123),
            header(2, 1) + b"\x81" + struct.pack("<Q", 0xFFFFFFFFFFFFFFFF),
            sparse() + b"\0", dense() + b"\0",
        ):
            with self.subTest(data=data):
                with self.assertRaises(audit.InvalidLibrary):
                    audit.parse_library(data)

    def test_sparse_delta_overflow(self):
        seed = b"\0" + struct.pack("<QQ", 0xFFFFFFFFFFFFFFFF, 0x1200)
        with self.assertRaises(audit.InvalidLibrary):
            audit.parse_library(header(2, 2) + seed + b"\x01" + struct.pack("<Q", 0x1300))

    def test_cli_checks_identity_coverage_and_evidence_limits(self):
        data = dense()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "versionlib-1-7-104-0.bin"
            path.write_bytes(data)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = audit.main([str(path), "--expect-version", "1.7.104.0", "--id", "1"])
            self.assertEqual(status, 0)
            report = json.loads(output.getvalue())
            self.assertEqual(report["sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(report["locations"][0]["base_rva"], "0x1234")
            for field in ("instructions_verified", "layouts_verified", "game_tested"):
                self.assertFalse(report[field])
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(audit.main([str(path), "--expect-version", "1.7.104.0", "--id", "2"]), 1)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(audit.main([str(path), "--expect-version", "1.6.1170.0", "--id", "1"]), 2)
            self.assertEqual(path.read_bytes(), data)

    def test_inventory_uses_explicit_family_and_covers_all_locations(self):
        for family, index in (("se", 1), ("ae", 2)):
            entries = tuple((identifier, 0x1000 + identifier * 8)
                for identifier in sorted({item[index] for item in audit.LOCATIONS}))
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "fixture.bin"
                path.write_bytes(sparse(entries=entries))
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    status = audit.main([str(path), "--expect-version", "1.7.104.0", "--family", family])
                self.assertEqual(status, 0)
                report = json.loads(output.getvalue())
                self.assertEqual(report["covered_locations"], 18)
                self.assertEqual(report["requested_locations"], 18)


if __name__ == "__main__":
    unittest.main()
