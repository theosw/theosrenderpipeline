"""Read-only Skyrim Address Library inspection; never establishes game support.

Formats 1/2 use compressed sparse ID/RVA pairs; format 5 uses a fixed header
and a dense uint32 RVA array indexed by ID. See docs/SKYRIM_1_7_104.md for the
pinned format references. This tool is independent of the plugin's loader.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys


class InvalidLibrary(ValueError):
    """The file cannot be interpreted as a supported x64 address table."""


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.position = 0

    @property
    def remaining(self) -> int:
        return len(self.data) - self.position

    def take(self, count: int) -> bytes:
        if count < 0 or count > self.remaining:
            raise InvalidLibrary(f"invalid or truncated field at byte {self.position}")
        start = self.position
        self.position += count
        return self.data[start:self.position]

    def number(self, fmt: str) -> int:
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))[0]


@dataclass
class AddressLibrary:
    format: int
    version: tuple[int, ...]
    image: str
    pointer_size: int
    entry_count: int
    data_format: int | None
    offsets: dict[int, int]

    def rva(self, identifier: int) -> int | None:
        # A dense zero slot denotes an absent ID, not an RVA at image base.
        return self.offsets.get(identifier) or None


def _compressed_value(reader: Reader, kind: int, previous: int) -> int:
    if kind == 0:
        value = reader.number("Q")
    elif kind == 1:
        value = previous + 1
    elif kind in (2, 3, 4, 5):
        delta = reader.number("B" if kind < 4 else "H")
        value = previous + delta if kind in (2, 4) else previous - delta
    elif kind in (6, 7):
        value = reader.number("H" if kind == 6 else "I")
    else:
        raise InvalidLibrary(f"unsupported compressed value type {kind}")
    if not 0 <= value <= 0xFFFFFFFFFFFFFFFF:
        raise InvalidLibrary("compressed value overflows uint64")
    return value


def parse_library(data: bytes) -> AddressLibrary:
    reader = Reader(data)
    format_version = reader.number("i")
    if format_version not in (1, 2, 5):
        raise InvalidLibrary(f"unsupported Address Library format {format_version}")
    version = tuple(reader.number("I") for _ in range(4))
    if any(component > 0xFFFF for component in version):
        raise InvalidLibrary("version component exceeds uint16")
    raw_name = reader.take(64 if format_version == 5 else reader.number("i"))
    try:
        name = raw_name.split(b"\0", 1)[0].decode("utf-8")
    except UnicodeDecodeError as error:
        raise InvalidLibrary("image name is not UTF-8") from error
    pointer_size = reader.number("i")
    if pointer_size != 8:
        raise InvalidLibrary(f"expected an x64 pointer size, got {pointer_size}")
    # Format 5's data-format field is reserved in the pinned reader. Retain it
    # in the report; it is not the pointer size or the offset count.
    data_format = reader.number("i") if format_version == 5 else None
    count = reader.number("i")
    minimum_entry_size = 4 if format_version == 5 else 1
    if count < 0 or count > reader.remaining // minimum_entry_size:
        raise InvalidLibrary("invalid or truncated entry count")

    offsets = {}
    previous_id = previous_offset = 0
    for index in range(count):
        if format_version == 5:
            identifier, offset = index, reader.number("I")
        else:
            tag = reader.number("B")
            identifier = _compressed_value(reader, tag & 15, previous_id)
            kind = tag >> 4
            scaled = (kind & 8) != 0
            offset = _compressed_value(reader, kind & 7,
                previous_offset // pointer_size if scaled else previous_offset)
            if scaled:
                offset *= pointer_size
            if offset > 0xFFFFFFFFFFFFFFFF:
                raise InvalidLibrary("scaled RVA overflows uint64")
            if identifier in offsets:
                raise InvalidLibrary(f"duplicate relocation ID {identifier}")
            previous_id, previous_offset = identifier, offset
        offsets[identifier] = offset
    if reader.remaining:
        raise InvalidLibrary(f"unexpected {reader.remaining} trailing bytes")
    return AddressLibrary(format_version, version, name, pointer_size, count,
        data_format, offsets)


# Inventory from the existing 1.5.97/1.6.640/1.6.1170 integrations. These are
# function/data IDs only: their intra-function hook offsets must be re-audited.
LOCATIONS = (
    ("DRS call", 35556, 36555),
    ("Cursor bounds", 50604, 51498),
    ("Screen size", 75590, 77397),
    ("Engine dimensions", 99938, 106583),
    ("Mist background", 51855, 52727),
    ("World completion", 79947, 82084),
    ("InitD3D call", 75595, 77226),
    ("GetClientRect call", 75460, 77245),
    ("UpdateJitter call", 75460, 77245),
    ("Jitter branch patch", 75709, 77518),
    ("Camera branch patch", 75711, 77520),
    ("RenderWorld call", 35560, 36559),
    ("Draw interface detour", 79947, 82084),
    ("Inventory3D detour", 50882, 51755),
    ("AllowTextInput function", 67252, 68552),
    ("TAA owner pointer", 527731, 414660),
    ("Graphics state", 524998, 411479),
    ("Artwork sender", 13214, 13363),
)


def _version_argument(value: str) -> tuple[int, ...]:
    try:
        version = tuple(int(component) for component in value.split("."))
        if len(version) == 4 and all(0 <= component <= 0xFFFF for component in version):
            return version
    except ValueError:
        pass
    raise argparse.ArgumentTypeError("use four uint16 components, e.g. 1.7.104.0")


def _id_argument(value: str) -> int:
    try:
        identifier = int(value, 0)
        if 0 <= identifier <= 0xFFFFFFFFFFFFFFFF:
            return identifier
    except ValueError:
        pass
    raise argparse.ArgumentTypeError("use a uint64 relocation ID")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("--expect-version", type=_version_argument, required=True)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--family", choices=("se", "ae"),
        help="explicitly select the existing SE or AE ID inventory")
    selection.add_argument("--id", type=_id_argument, action="append",
        help="inspect specific IDs instead of the renderer inventory; repeatable")
    args = parser.parse_args(argv)
    try:
        data = args.library.read_bytes()
        library = parse_library(data)
        if library.version != args.expect_version:
            raise InvalidLibrary(f"expected {args.expect_version}, found {library.version}")
    except (OSError, InvalidLibrary) as error:
        print(f"Address Library audit failed: {error}", file=sys.stderr)
        return 2

    selected = ([(f"ID {identifier}", identifier) for identifier in args.id]
        if args.id else [(item[0], item[1 if args.family == "se" else 2]) for item in LOCATIONS])
    locations = [{"name": name, "id": identifier,
        "base_rva": hex(offset) if (offset := library.rva(identifier)) is not None else None}
        for name, identifier in selected]
    covered = sum(item["base_rva"] is not None for item in locations)
    print(json.dumps({
        "file": args.library.name,
        "sha256": hashlib.sha256(data).hexdigest(),
        "format": library.format,
        "version": list(library.version),
        "image": library.image,
        "pointer_size": library.pointer_size,
        "data_format": library.data_format,
        "entry_count": library.entry_count,
        "covered_locations": covered,
        "requested_locations": len(locations),
        "instructions_verified": False,
        "layouts_verified": False,
        "game_tested": False,
        "locations": locations,
    }, indent=2))
    return 0 if covered == len(locations) else 1


if __name__ == "__main__":
    sys.exit(main())
