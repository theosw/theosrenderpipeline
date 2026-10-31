"""Build-time CUDA ELF launch bounds; no ELF parsing is needed in the plugin."""
import math
import re
import struct


def extract(cubin, names, sm):
    def need(ok, why):
        if not ok:
            raise RuntimeError('native cubin bounds: ' + why)
    def value(fmt, at):
        need(0 <= at <= len(cubin) - struct.calcsize(fmt), 'truncated ELF')
        return struct.unpack_from(fmt, cubin, at)
    need(cubin[:6] == b'\x7fELF\x02\x01', 'expected little-endian ELF64')
    need(value('<H', 18)[0] == 190 and value('<I', 48)[0] & 255 == sm, 'ELF architecture differs')
    shoff, = value('<Q', 40)
    stride, count, strings_index = value('<HHH', 58)
    need(stride == 64 and strings_index < count and shoff + count*stride <= len(cubin), 'invalid section table')
    def section(i):
        name, kind, flags, addr, offset, size, link, info, align, entsize = value('<IIQQQQIIQQ', shoff+i*stride)
        need(kind == 8 or offset + size <= len(cubin), 'section outside ELF')
        return name, kind, offset, size, info
    _, _, offset, size, _ = section(strings_index)
    strings = cubin[offset:offset+size]
    sections = {}
    for i in range(count):
        n, kind, offset, size, info = section(i)
        need(n < len(strings) and b'\0' in strings[n:], 'invalid section name')
        name = strings[n:].split(b'\0', 1)[0].decode('ascii')
        sections[name] = (kind, offset, size, info)
    result = {}
    for name in names:
        need('.text.'+name in sections, 'missing text for '+name)
        _, _, _, info = sections['.text.'+name]
        regs = info >> 24
        need(regs <= 255, 'invalid register count for '+name)
        attrs = {}
        if '.nv.info.'+name in sections:
            _, at, size, _ = sections['.nv.info.'+name]
            end = at+size
            while at < end:
                need(end-at >= 4, 'truncated attribute')
                fmt, attr, size = value('<BBH', at)
                need(1 <= fmt <= 4, 'unknown attribute format')
                at += 4
                if fmt == 4:
                    need(size <= end-at, 'truncated attribute value')
                    if attr in (5, 16):  # EIATTR_MAX_THREADS, EIATTR_REQNTID
                        need(size == 12 and attr not in attrs, 'invalid thread attribute')
                        attrs[attr] = list(value('<III', at))
                        need(all(1 <= x <= lim for x,lim in zip(attrs[attr], (1024,1024,64))) and math.prod(attrs[attr]) <= 1024, 'invalid thread dimensions')
                    at += size
        # cuda_occupancy.h: 256 registers per warp allocation unit and four
        # register-file partitions on SM61/89; launch checks round warps to four.
        per_warp = ((regs*32+255)//256)*256
        register_limit = min(1024, (65536//(per_warp*4))*4*32) if per_warp else 1024
        maximum = min(register_limit, math.prod(attrs.get(5, [1024,1,1])))
        static_shared = sections.get('.nv.shared.'+name, (0,0,0,0))[2]
        need(maximum > 0 and static_shared <= 49152, 'kernel cannot launch: '+name)
        required = attrs.get(16, [0,0,0])
        need(not required[0] or math.prod(required) <= maximum, 'reqntid exceeds resources: '+name)
        result[name] = dict(maxThreads=maximum, reqntid=required, maxDynamicShared=49152-static_shared,
                            registers=regs, staticShared=static_shared, sm=sm)
    return result


def original_blocks(ptx):
    result = {}
    for match in re.finditer(r'\.entry\s+(\w+)\s*\([^)]*\)\s*([^\{]*)\{', ptx):
        header = match[2]
        dims = (re.search(r'\.reqntid\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)', header) or
                re.search(r'\.maxntid\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)', header))
        if dims:
            result[match[1]] = list(map(int, dims.groups()))
    return result


def validate(name, bound, original, rule):
    block = rule['block'] if rule else original
    if block is None:
        return
    shared = rule.get('shared') if rule else None
    if (math.prod(block) > bound['maxThreads'] or
            (bound['reqntid'][0] and block != bound['reqntid']) or
            (shared is not None and shared > bound['maxDynamicShared'])):
        raise RuntimeError(f'native kernel {name} sm_{bound["sm"]}: block {block} or shared {shared} exceeds compiled launch bounds {bound}; supply a valid --launch rule')


def add_records(rules, bundles):
    """Merge two architecture-specific bounds into schema-2 launch records."""
    merged = {r['name']: dict(r) for r in rules}
    for bundle, arch_bounds in bundles.items():
        for arch, bounds in arch_bounds.items():
            for name, bound in bounds.items():
                r = merged.setdefault(name, dict(name=name, bundle=bundle, architectures=3,
                                                 block=[0,0,0], grid=[0,0,0], shared=None))
                r.setdefault('bounds', {})[str(arch)] = bound
    return sorted(merged.values(), key=lambda r: (r['bundle'], r['name']))
