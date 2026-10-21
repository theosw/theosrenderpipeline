"""TRPNRK1 v2: unchanged v1 bundle table/payloads, followed by required launch records."""
import re
import struct


def parse(specs, native):
    names = {}
    for bundle, sources in native.items():
        for source in sources:
            for name in re.findall(r'\.entry\s+(\w+)', source):
                if name in names:
                    raise RuntimeError(f'duplicate native entry: {name}')
                names[name] = bundle
    rules = []
    seen = set()
    for spec in specs:
        fields = spec.split(':')
        if len(fields) < 2 or len(fields) > 4 or fields[0] not in names or fields[0] in seen:
            raise RuntimeError(f'invalid, duplicate or non-native launch requirement: {spec}')
        def dim(text, limits):
            if not re.fullmatch(r'[0-9]+x[0-9]+x[0-9]+', text):
                raise RuntimeError(f'invalid launch dimensions: {spec}')
            values = list(map(int, text.split('x')))
            if any(v < 1 or v > limit for v, limit in zip(values, limits)):
                raise RuntimeError(f'launch dimensions exceed limits: {spec}')
            return values
        block = dim(fields[1], (1024, 1024, 64))
        if block[0] * block[1] * block[2] > 1024:
            raise RuntimeError(f'block exceeds 1024 threads: {spec}')
        grid = dim(fields[2], (2147483647, 65535, 65535)) if len(fields) >= 3 and fields[2] else [0, 0, 0]
        if len(fields) == 3 and not fields[2]:
            raise RuntimeError(f'empty grid: {spec}')
        shared = None
        if len(fields) == 4:
            if not re.fullmatch(r'[0-9]+', fields[3]) or int(fields[3]) > 49152:
                raise RuntimeError(f'invalid dynamic shared bytes: {spec}')
            shared = int(fields[3])
        seen.add(fields[0])
        rules.append(dict(name=fields[0], bundle=names[fields[0]], architectures=3,
                          block=block, grid=grid, shared=shared))
    return sorted(rules, key=lambda r: (r['bundle'], r['name']))


def append(pack, rules):
    if not rules:
        return pack                 # v1 remains byte-for-byte reproducible
    suffix = bytearray(b'TRPLCH1\0' + struct.pack('<II', 1, len(rules)))
    for r in rules:
        name = r['name'].encode('ascii')
        flags = int(bool(r['grid'][0])) | (2 if r['shared'] is not None else 0)
        suffix += struct.pack('<11I', r['bundle'], r['architectures'], len(name), flags,
                              *r['block'], *r['grid'], r['shared'] or 0)
        suffix += name + b'\0' * (-len(name) % 4)
    return pack[:8] + struct.pack('<I', 2) + pack[12:] + suffix
