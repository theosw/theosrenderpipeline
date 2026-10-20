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


def emulation_functions(emulation_ptx):
    """The .func definitions of pascal_emulation.ptx, made module-local."""
    body = emulation_ptx.replace('\r', '')
    start = body.index('.visible .func')
    functions = body[start:].replace('.visible .func', '.func')
    require(functions.count('.func') == 7, 'pascal_emulation.ptx does not hold the seven stand-ins')
    return '\n// PASCAL: stand-ins for sm_75+ instructions (pascal_emulation.cu)\n' + functions + '\n'


def lower(ada_ptx, emulation_ptx):
    counts = collections.Counter()
    s = ada_ptx.replace('\r', '')
    entries = re.findall(r'\.entry\s+(\w+)', s)
    require(len(re.findall(r'\.target sm_89\b', s)) == 1, 'expected one sm_89 target')
    s = re.sub(r'\.version 9\.\d+', '.version 8.8', s)
    s = s.replace('.target sm_89', '.target sm_61')
    address = re.search(r'\.address_size 64\n', s)
    require(address, 'expected .address_size 64')
    s = s[:address.end()] + emulation_functions(emulation_ptx) + s[address.end():]

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
    for op in ('min', 'max'):
        s = sub(f'{op}.f16x2', op + r'\.f16x2\s+' + R + SEP + R + SEP + R + r'\s*;',
                lambda m, op=op: call(f'trp_{op}_f16x2', [m[2], m[3]], [m[1]]), s)
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
