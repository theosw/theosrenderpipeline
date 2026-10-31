"""Stage the pinned Pascal asset in a NEW Universal package supplement.

No binaries are fetched, built, committed or deployed. The package assembler must
merge this additive inventory with the existing Universal payload, retain its RTX
compatibility assets, and verify the required original runtime in the merged view.
"""
import argparse
import hashlib
import json
from pathlib import Path

MANIFEST = Path(__file__).resolve().parents[2] / 'package/NR-CompatibilityAssets.json'


def stage(source: Path, output: Path, edition: str) -> None:
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    if edition != manifest['edition']:
        raise ValueError('Pascal compatibility kernels are Universal only; Standard must not install them')
    data = source.read_bytes()
    if len(data) != manifest['size'] or hashlib.sha256(data).hexdigest().upper() != manifest['sha256']:
        raise ValueError('Pascal asset size/SHA-256 differs from the pinned inventory')
    if output.exists():
        raise ValueError('Choose a new package supplement output directory')
    destination = output / manifest['path']
    destination.parent.mkdir(parents=True)
    destination.write_bytes(data)
    if destination.read_bytes() != data:
        raise ValueError('Staged Pascal pack readback differs')
    print(f'Verified Universal supplement: {destination}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='Pinned local sm-general-pascal.trpk input')
    parser.add_argument('--output', type=Path, required=True, help='New Universal staging directory')
    parser.add_argument('--edition', choices=['Standard', 'Universal'], required=True)
    args = parser.parse_args()
    stage(args.input.resolve(), args.output.resolve(), args.edition)
