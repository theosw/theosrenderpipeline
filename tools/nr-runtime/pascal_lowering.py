"""Research: NVIDIA NR 310.9.16 network PTX, already lowered for Ada (ada_lowering.py), ->
Pascal (sm_61). Run with python -I.

Pascal has no tensor cores, FP8 conversions, movmatrix, cp.async or the sm_70 memory
model. Each such instruction becomes a call to a stand-in from pascal_emulation.cu (compiled
to sm_61 PTX by the caller) or a direct pre-Volta equivalent:
    mma m16n8k32 e4m3 / m16n8k16 f16  -> trp_mma_e4m3 / trp_mma_f16 (warp shuffles, f32 FMA)
    cvt e4m3x2 <-> f16x2              -> trp_cvt_* (exact, RNE + satfinite)
    movmatrix .trans                  -> trp_movmatrix_trans (two shuffles)
    min/max.f16x2                     -> trp_min_f16x2 / trp_max_f16x2
    cp.async 4/16 bytes               -> synchronous ld.global + st.shared; commit/wait removed
    ld.weak                           -> ld
    ld.relaxed.gpu / st.release.gpu   -> ld.volatile / membar.gl + st.volatile
    fence.acq_rel.gpu                 -> membar.gl
    nanosleep                         -> removed (plain spin)
    st.global .b128 via mov.b128      -> st.global.v4.b32
The stand-ins match the native instructions bit for bit except the mma accumulation, which
rounds once in f32 instead of following the tensor cores' narrower accumulator (closer to the
exact result). Fail-closed: anything left that sm_61 cannot run is an error.
"""
import collections
import re

import pascal_groups


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


R = r'(%\w+)'
SEP = r'\s*,\s*'
MMA = {
    'mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16': 'trp_mma_e4m3',
    'mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16': 'trp_mma_f16',
}
LEFTOVER = re.compile(r'mma\.|movmatrix|e4m3|e5m2|cp\.async|\.weak|\.relaxed|\.release|\.acq_rel|fence\.|nanosleep|\.b128|'
                      r'(?:min|max)\.f16|::no_allocate|ldmatrix|stmatrix|redux\.|elect\.|mbarrier')


def call(function, arguments, results, packed_result=False):
    """PTX call sequence in its own scope. `results` are destination registers."""
    lines = ['{']
    for i, value in enumerate(arguments):
        lines += [f'.param .b32 trp_a{i};', f'st.param.b32 [trp_a{i}], {value};']
    if packed_result:
        lines.append(f'.param .align 4 .b8 trp_ret[{4 * len(results)}];')
    else:
        lines.append('.param .b32 trp_ret;')
    lines.append(f'call (trp_ret), {function}, ({", ".join(f"trp_a{i}" for i in range(len(arguments)))});')
    for i, register in enumerate(results):
        lines.append(f'ld.param.b32 {register}, [trp_ret+{4 * i}];')
    lines.append('}')
    return '\n'.join(lines)


def half2_binary(f32, d, a, b):
    """f16x2 d = op(a, b) through f32, rounded to nearest even per half."""
    return ('{\n.reg .b16 trp_h<4>;\n.reg .f32 trp_f<4>;\n'
            f'mov.b32 {{trp_h0, trp_h1}}, {a};\nmov.b32 {{trp_h2, trp_h3}}, {b};\n'
            'cvt.f32.f16 trp_f0, trp_h0;\ncvt.f32.f16 trp_f1, trp_h1;\ncvt.f32.f16 trp_f2, trp_h2;\ncvt.f32.f16 trp_f3, trp_h3;\n'
            f'{f32} trp_f0, trp_f0, trp_f2;\n{f32} trp_f1, trp_f1, trp_f3;\n'
            'cvt.rn.f16.f32 trp_h0, trp_f0;\ncvt.rn.f16.f32 trp_h1, trp_f1;\n'
            f'mov.b32 {d}, {{trp_h0, trp_h1}};\n}}')


def half_binary(f32, d, a, b):
    return ('{\n.reg .f32 trp_f<2>;\n'
            f'cvt.f32.f16 trp_f0, {a};\ncvt.f32.f16 trp_f1, {b};\n{f32} trp_f0, trp_f0, trp_f1;\n'
            f'cvt.rn.f16.f32 {d}, trp_f0;\n}}')


def half2_fma(d, a, b, c):
    return ('{\n.reg .b16 trp_h<6>;\n.reg .f32 trp_f<6>;\n'
            f'mov.b32 {{trp_h0, trp_h1}}, {a};\nmov.b32 {{trp_h2, trp_h3}}, {b};\nmov.b32 {{trp_h4, trp_h5}}, {c};\n'
            + ''.join(f'cvt.f32.f16 trp_f{i}, trp_h{i};\n' for i in range(6)) +
            'fma.rn.f32 trp_f0, trp_f0, trp_f2, trp_f4;\nfma.rn.f32 trp_f1, trp_f1, trp_f3, trp_f5;\n'
            'cvt.rn.f16.f32 trp_h0, trp_f0;\ncvt.rn.f16.f32 trp_h1, trp_f1;\n'
            f'mov.b32 {d}, {{trp_h0, trp_h1}};\n}}')


def emulation_functions(emulation_ptx):
    """The .func definitions of pascal_emulation.ptx, made module-local."""
    body = emulation_ptx.replace('\r', '')
    start = body.index('.visible .func')
    functions = body[start:].replace('.visible .func', '.func')
    require(functions.count('.func') == 7, 'pascal_emulation.ptx does not hold the seven stand-ins')
    return '\n// PASCAL: stand-ins for sm_75+ instructions (pascal_emulation.cu)\n' + functions + '\n'


def group_functions(groups_ptx):
    """The .func definitions of the generated grouped stand-ins (pascal_groups.py), module-local."""
    body = groups_ptx.replace('\r', '')
    return ('\n// PASCAL: grouped mma stand-ins (pascal_groups.py)\n' +
            body[body.index('.visible .func'):].replace('.visible .func', '.func') + '\n')


def lower(ada_ptx, emulation_ptx, groups_ptx=None):
    """groups_ptx: compiled pascal_groups.generate_cu() output covering this module's runs, or
    None to keep one stand-in call per mma."""
    counts = collections.Counter()
    s = ada_ptx.replace('\r', '')
    entries = re.findall(r'\.entry\s+(\w+)', s)
    require(len(re.findall(r'\.target sm_89\b', s)) == 1, 'expected one sm_89 target')
    s = re.sub(r'\.version 9\.\d+', '.version 8.8', s)
    s = s.replace('.target sm_89', '.target sm_61')
    address = re.search(r'\.address_size 64\n', s)
    require(address, 'expected .address_size 64')
    s = s[:address.end()] + emulation_functions(emulation_ptx) + s[address.end():]
    if groups_ptx:
        s, counts['grouped mma'] = pascal_groups.replace_runs(s, call)
        address = re.search(r'\.address_size 64\n', s)
        s = s[:address.end()] + group_functions(groups_ptx) + s[address.end():]

    def sub(name, pattern, replace, text, flags=0):
        result, n = re.subn(pattern, replace, text, flags=flags)
        counts[name] += n
        return result

    for instruction, function in MMA.items():
        pattern = (re.escape(instruction) + r'\s*\{\s*' + R + SEP + R + r'\s*\}' + SEP +
                   r'\{\s*' + R + SEP + R + SEP + R + SEP + R + r'\s*\}' + SEP +
                   r'\{\s*' + R + SEP + R + r'\s*\}' + SEP + r'\{\s*' + R + SEP + R + r'\s*\}\s*;')
        s = sub(function, pattern, lambda m, f=function: call(f, m.groups()[2:], m.groups()[:2], packed_result=True), s)
    s = sub('cvt encode', r'cvt\.rn\.satfinite\.e4m3x2\.f16x2\s+' + R + SEP + R + r'\s*;',
            lambda m: '{\n.reg .b32 trp_t;\n' + call('trp_cvt_e4m3x2_f16x2', [m[2]], ['trp_t']) +
                      f'\ncvt.u16.u32 {m[1]}, trp_t;\n}}', s)
    s = sub('cvt decode', r'cvt\.rn\.f16x2\.e4m3x2\s+' + R + SEP + R + r'\s*;',
            lambda m: f'{{\n.reg .b32 trp_t;\ncvt.u32.u16 trp_t, {m[2]};\n' +
                      call('trp_cvt_f16x2_e4m3x2', ['trp_t'], [m[1]]) + '\n}', s)
    s = sub('movmatrix', r'movmatrix\.sync\.trans\.aligned\.m8n8\.b16\s+' + R + SEP + R + r'\s*;',
            lambda m: call('trp_movmatrix_trans', [m[2]], [m[1]]), s)
    # Half-precision arithmetic runs at 1/64 rate on GP10x (2 results per clock per SM).
    # Compute in f32 and round once to f16: exact for add, sub, mul, min and max (f16
    # products and sums of nearby f16 values fit f32); fma can differ by one f16 ulp in
    # rare double-rounding cases.
    for op, f32 in (('min', 'min.f32'), ('max', 'max.f32'), ('mul', 'mul.rn.f32'), ('add', 'add.rn.f32'),
                    ('sub', 'sub.rn.f32')):
        s = sub(f'{op}.f16x2', r'\b' + op + r'\.f16x2\s+' + R + SEP + R + SEP + R + r'\s*;',
                lambda m, f32=f32: half2_binary(f32, m[1], m[2], m[3]), s)
        s = sub(f'{op}.f16', r'\b' + op + r'\.f16\s+' + R + SEP + R + SEP + R + r'\s*;',
                lambda m, f32=f32: half_binary(f32, m[1], m[2], m[3]), s)
    s = sub('fma.f16x2', r'\bfma\.rn\.f16x2\s+' + R + SEP + R + SEP + R + SEP + R + r'\s*;',
            lambda m: half2_fma(m[1], m[2], m[3], m[4]), s)
    s = sub('abs.f16x2', r'\babs\.f16x2\s+' + R + SEP + R + r'\s*;', lambda m: f'and.b32 {m[1]}, {m[2]}, 0x7FFF7FFF;', s)
    s = sub('cp.async 16', r'cp\.async\.ca\.shared\.global\s+\[([^\]]+)\]' + SEP + r'\[([^\]]+)\]' + SEP + r'16\s*;',
            lambda m: ('{\n.reg .b32 trp_c<4>;\n'
                       f'ld.global.ca.v4.b32 {{trp_c0, trp_c1, trp_c2, trp_c3}}, [{m[2]}];\n'
                       f'st.shared.v4.b32 [{m[1]}], {{trp_c0, trp_c1, trp_c2, trp_c3}};\n}}'), s)
    s = sub('cp.async 4', r'cp\.async\.ca\.shared\.global\s+\[([^\]]+)\]' + SEP + r'\[([^\]]+)\]' + SEP + r'4\s*;',
            lambda m: f'{{\n.reg .b32 trp_c;\nld.global.ca.b32 trp_c, [{m[2]}];\nst.shared.b32 [{m[1]}], trp_c;\n}}', s)
    s = sub('cp.async commit', r'cp\.async\.commit_group\s*;', '// PASCAL: copies above completed synchronously.', s)
    s = sub('cp.async wait', r'cp\.async\.wait_group\s+\d+\s*;', '// PASCAL: copies above completed synchronously.', s)
    s = sub('ld.weak', r'ld\.weak\.global\.', 'ld.global.', s)
    s = sub('ld.relaxed', r'ld\.relaxed\.gpu\.global\.L1::no_allocate\.', 'ld.volatile.global.', s)
    s = sub('st.release', r'st\.release\.gpu\.global\.L1::no_allocate\.(\w+)\s+([^;]+);',
            lambda m: f'membar.gl;\nst.volatile.global.{m[1]} {m[2]};', s)
    s = sub('fence', r'fence\.acq_rel\.gpu\s*;', 'membar.gl;', s)
    s = sub('nanosleep', r'nanosleep\.u32\s+%\w+\s*;', '// PASCAL: spin without nanosleep.', s)
    s = sub('st.b128', r'\{\s*\.reg \.b128 v;\s*mov\.b128 v,\s*\{' + R + SEP + R + SEP + R + SEP + R + r'\}\s*;\s*'
            r'st\.global\.L1::no_allocate\.b128\s+\[([^\]]+)\]\s*,\s*v\s*;\s*\}',
            lambda m: f'st.global.v4.b32 [{m[5]}], {{{m[1]}, {m[2]}, {m[3]}, {m[4]}}};', s)

    module_body = s[s.index('// PASCAL: stand-ins'):]
    module_body = module_body[module_body.index('.entry') if '.entry' in module_body else 0:]
    plain = re.sub(r'trp_\w+', '', re.sub(r'//[^\n]*', '', module_body))
    leftover = sorted({m.group(0) for m in LEFTOVER.finditer(plain)})
    require(not leftover, f'unlowered sm_70+ features remain: {leftover}')
    require(re.findall(r'\.entry\s+(\w+)', s) == entries, 'kernel entries changed')
    return s, dict(counts)


def replace_entries(module_ptx, native_ptx):
    """Swaps NVIDIA kernels for native ones with the same name (and parameter block).
    native_ptx: nvcc output for sm_61; everything after .address_size is appended."""
    native = native_ptx.replace('\r', '')
    body = native[native.index('.address_size 64') + len('.address_size 64'):]
    names = re.findall(r'\.entry\s+(\w+)\s*\(', body)
    require(names, 'native PTX has no kernels')
    s = module_ptx
    for name in names:
        start = re.search(r'(?:\.visible\s+)?\.entry\s+' + name + r'\s*\(', s)
        require(start, f'module has no kernel {name}')
        native_params = re.search(r'\.entry\s+' + name + r'\s*\(([^)]*)\)', body).group(1)
        module_params = re.search(r'\.entry\s+' + name + r'\s*\(([^)]*)\)', s).group(1)
        size = lambda text: re.findall(r'\.align\s+(\d+)\s+\.b8\s+\w+\[(\d+)\]', text)
        require(size(native_params) == size(module_params), f'{name}: parameter blocks differ')
        open_brace = s.index('{', s.index(')', start.end()))
        depth, i = 0, open_brace
        while True:
            depth += {'{': 1, '}': -1}.get(s[i], 0)
            if depth == 0:
                break
            i += 1
        s = s[:start.start()] + f'// PASCAL: {name} replaced by a native kernel below.\n' + s[i + 1:]
    return s + '\n// PASCAL: native kernels (native_*.cu)\n' + body, names
