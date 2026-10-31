"""Research: groups runs of FP8 mma instructions in NR's network PTX into one stand-in call.

NVIDIA's kernels issue mma.m16n8k32.e4m3 in runs: outer products of a few A fragments
(token tiles) with a few B fragments (channel tiles), sometimes chained along K. The
per-instruction stand-in (trp_mma_e4m3) shuffles in and decodes B for every instruction, so
a B used with four A tiles is fetched and decoded four times. Here each run is split into
blocks of at most four A and four B fragments; a generated function per block shape fetches
and decodes each B once and reuses it for every A. Each output is computed with exactly the
arithmetic of trp_mma_e4m3 (same products, same summation order, same f16 rounding), so the
results are bit-identical to the per-instruction stand-in.

Instructions between the mma of a run (register moves and loads) are hoisted in front of
the calls; runs where that would change what any instruction reads or writes are split.

Measured 2026-10-20 (build_pascal_kernels.py --groups): bit-identical output, and the RTX 4080
frame drops from 43 to 30 ms, but the GTX 1070 Ti gets no faster (4x4 blocks: 211 -> 311 ms;
2x4: 210 ms). Under NVIDIA's .maxnreg 168 the wide call interface spills to local memory, and
without the cap occupancy drops; inlining makes sm_61 register allocation spill even more. In
isolation (lower/mma_bench) one window of mma takes 19.5 ms per-mma, 28-94 ms grouped, against a
5.3 ms plain-FMA floor. Kept as an option for reference; off by default.
"""
import hashlib
import re


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


E4M3 = 'mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16'
REG = r'%\w+'
MMA = re.compile(re.escape(E4M3) + r'\s*\{([^}]*)\}\s*,\s*\{([^}]*)\}\s*,\s*\{([^}]*)\}\s*,\s*\{([^}]*)\}\s*;')
GAP_OPS = ('mov', 'ld', 'cvt', 'prmt')
MAX_A, MAX_B = 4, 4


def regs(text):
    return tuple(x.strip() for x in text.split(','))


def split_operands(instruction):
    """(defs, uses) register sets of one gap instruction: the first operand is written."""
    body = instruction.strip().rstrip(';')
    parts = body.split(None, 1)
    if len(parts) < 2:
        return set(), set()
    operands, depth, current = [], 0, ''
    for ch in parts[1]:
        if ch in '{[':
            depth += 1
        elif ch in '}]':
            depth -= 1
        if ch == ',' and depth == 0:
            operands.append(current); current = ''
        else:
            current += ch
    operands.append(current)
    defs = set(re.findall(REG, operands[0]))
    uses = set(re.findall(REG, ','.join(operands[1:])))
    return defs, uses


class Mma:
    def __init__(self, match):
        self.d, self.a, self.b, self.c = (regs(match.group(i)) for i in range(1, 5))
        self.start, self.end = match.start(), match.end()


def gap_instructions(text):
    """Instructions of a gap, or None if it holds anything other than plain mov/ld/cvt/prmt."""
    instructions = []
    for raw in text.split(';'):
        line = re.sub(r'//[^\n]*', '', raw).strip()
        if not line:
            continue
        if any(ch in line for ch in '{}:@') or line.split()[0].split('.')[0] not in GAP_OPS:
            return None
        instructions.append(line + ';')
    return instructions


def find_runs(body):
    """Runs of consecutive e4m3 mma: lists of (mma, gap instructions before it)."""
    mmas = [Mma(m) for m in MMA.finditer(body)]
    runs, current = [], []
    for m in mmas:
        if current:
            gap = gap_instructions(body[current[-1][0].end:m.start])
            ok = gap is not None
            if ok:
                earlier_operands = {r for mm, _ in current for r in mm.a + mm.b + mm.c + mm.d}
                earlier_d = {r for mm, _ in current for r in mm.d}
                for instruction in gap:
                    defs, uses = split_operands(instruction)
                    if defs & earlier_operands or uses & earlier_d:
                        ok = False
                        break
            if ok and set(m.a + m.b) & {r for mm, _ in current for r in mm.d}:
                ok = False
            if ok:
                current.append((m, gap))
                continue
            runs.append(current)
        current = [(m, [])]
    if current:
        runs.append(current)
    return runs


def plan(run):
    """Blocks for a run, or None when it cannot be grouped. A block is a list of run indices;
    blocks are emitted in order and each gets one call."""
    mmas = [m for m, _ in run]
    if len(mmas) < 2:
        return None
    a_index, b_index = {}, {}
    for m in mmas:
        a_index.setdefault(m.a, len(a_index)); b_index.setdefault(m.b, len(b_index))
    # Original semantics: the value each mma reads as C is either the pre-run value of the
    # registers (None) or the result of an earlier mma in the run (its index).
    writer, source = {}, []
    for i, m in enumerate(mmas):
        sources = {writer.get(r) for r in m.c}
        if len(sources) != 1:
            return None
        source.append(sources.pop())
        for r in m.d:
            writer[r] = i
    keys = sorted({(a_index[m.a] // MAX_A, b_index[m.b] // MAX_B) for m in mmas},
                  key=lambda k: min(i for i, m in enumerate(mmas) if (a_index[m.a] // MAX_A, b_index[m.b] // MAX_B) == k))
    blocks = [[i for i, m in enumerate(mmas) if (a_index[m.a] // MAX_A, b_index[m.b] // MAX_B) == k] for k in keys]
    # Replay in emission order. A call reads the C of its mma whose source lies outside the
    # block at entry, takes in-block sources from its own results, and writes all D in index
    # order. Every mma must read the same C as in the original order.
    writer = {}
    for block in blocks:
        entry = dict(writer)
        for i in block:
            if source[i] not in block and {entry.get(r) for r in mmas[i].c} != {source[i]}:
                return None
        for i in block:
            for r in mmas[i].d:
                writer[r] = i
    # Final register state must match the original's last writers.
    final = {}
    for i, m in enumerate(mmas):
        for r in m.d:
            final[r] = i
    return blocks if writer == final else None


def block_signature(mmas, block, source):
    """(nA, nB, ((a, b, c), ...)): local indices; c is -1 for an external C, else the local
    index of the source mma within the block."""
    a_local, b_local, spec = {}, {}, []
    for i in block:
        m = mmas[i]
        a = a_local.setdefault(m.a, len(a_local)); b = b_local.setdefault(m.b, len(b_local))
        c = block.index(source[i]) if source[i] is not None and source[i] in block else -1
        spec.append((a, b, c))
    return len(a_local), len(b_local), tuple(spec)


def function_name(signature):
    return 'trp_g_' + hashlib.sha256(repr(signature).encode()).hexdigest()[:12]


def generate_function(signature):
    """CUDA C++ for one block shape: identical per-output arithmetic to trp_mma_e4m3."""
    n_a, n_b, spec = signature
    name = function_name(signature)
    externals = [k for k, (_, _, c) in enumerate(spec) if c < 0]
    args = ([f'unsigned a{i}_{r}' for i in range(n_a) for r in range(4)] +
            [f'unsigned b{j}_{r}' for j in range(n_b) for r in range(2)] +
            [f'unsigned c{k}_{r}' for k in externals for r in range(2)])
    out = [f'struct {name}_ret {{ unsigned d[{2 * len(spec)}]; }};',
           f'extern "C" __device__ __noinline__ {name}_ret {name}({", ".join(args)})', '{',
           '    const unsigned t = LaneId() & 3;']
    for i in range(n_a):
        out.append(f'    float lo{i}[8], hi{i}[8]; DecodeA(a{i}_0, a{i}_1, a{i}_2, a{i}_3, lo{i}, hi{i});')
    out.append(f'    float own[{len(spec)}][2][2];')
    out.append('    ' + name + '_ret r;')
    finished = set()

    def finish_ready(reduced):
        # Each result is packed as soon as its own sum and its C are available.
        progress = True
        while progress:
            progress = False
            for k, (_, _, c) in enumerate(spec):
                if k in finished or k not in reduced or (c >= 0 and c not in finished):
                    continue
                cx, cy = (f'c{k}_0', f'c{k}_1') if c < 0 else (f'r.d[{2 * c}]', f'r.d[{2 * c + 1}]')
                out.append(f'    r.d[{2 * k}] = Finish(own[{k}][0], {cx}); r.d[{2 * k + 1}] = Finish(own[{k}][1], {cy});')
                finished.add(k)
                progress = True

    reduced = set()
    for j in range(n_b):
        users = [k for k, (_, b, _) in enumerate(spec) if b == j]
        out.append('    {')
        out.append(f'        float p[{len(users)}][2][8];')
        out.append('#pragma unroll')
        out.append('        for (unsigned col = 0; col < 8; ++col) {')
        out.append('            const unsigned from = (col << 2) | t;')
        out.append(f'            const unsigned k0 = __shfl_sync(0xFFFFFFFFu, b{j}_0, from), k1 = __shfl_sync(0xFFFFFFFFu, b{j}_1, from);')
        out.append('            float bk0[4], bk1[4];')
        out.append('#pragma unroll')
        out.append('            for (unsigned q = 0; q < 4; ++q) { bk0[q] = E4M3(k0 >> (8 * q)); bk1[q] = E4M3(k1 >> (8 * q)); }')
        for u, k in enumerate(users):
            a = spec[k][0]
            out.append(f'            p[{u}][0][col] = Dot(lo{a}, bk0, bk1); p[{u}][1][col] = Dot(hi{a}, bk0, bk1);')
        out.append('        }')
        for u, k in enumerate(users):
            out.append(f'        ReduceScatter(p[{u}], own[{k}], t);')
        out.append('    }')
        reduced.update(users)
        finish_ready(reduced)
    require(len(finished) == len(spec), 'block has a C source cycle')
    out.append('    return r;')
    out.append('}')
    return '\n'.join(out)


HELPERS = r'''// Generated by pascal_groups.py. Research: grouped FP8 mma stand-ins for sm_61.
#include <cuda_fp16.h>
namespace
{
    __device__ __forceinline__ unsigned LaneId()
    {
        unsigned lane;
        asm volatile("mov.u32 %0, %%laneid;" : "=r"(lane));
        return lane;
    }
    __device__ __forceinline__ float E4M3(unsigned bits)
    {
        return __uint_as_float(((bits & 0x80u) << 24) | ((bits & 0x7Fu) << 20)) * 1.329227995784916e36f;
    }
    __device__ __forceinline__ unsigned PackHalf2(float low, float high)
    {
        return static_cast<unsigned>(__half_as_ushort(__float2half_rn(low))) |
            (static_cast<unsigned>(__half_as_ushort(__float2half_rn(high))) << 16);
    }
    __device__ __forceinline__ float LowHalf(unsigned value) { return __half2float(__ushort_as_half(static_cast<unsigned short>(value))); }
    __device__ __forceinline__ float HighHalf(unsigned value) { return __half2float(__ushort_as_half(static_cast<unsigned short>(value >> 16))); }
    // A rows g and g+8 at this lane's 8 k, as in trp_mma_e4m3.
    __device__ __forceinline__ void DecodeA(unsigned a0, unsigned a1, unsigned a2, unsigned a3, float (&lo)[8], float (&hi)[8])
    {
#pragma unroll
        for (unsigned q = 0; q < 4; ++q) {
            lo[q] = E4M3(a0 >> (8 * q)); lo[4 + q] = E4M3(a2 >> (8 * q));
            hi[q] = E4M3(a1 >> (8 * q)); hi[4 + q] = E4M3(a3 >> (8 * q));
        }
    }
    __device__ __forceinline__ float Dot(const float (&a)[8], const float (&bk0)[4], const float (&bk1)[4])
    {
        float sum = 0.0f;
#pragma unroll
        for (unsigned q = 0; q < 4; ++q) { sum = fmaf(a[q], bk0[q], fmaf(a[4 + q], bk1[q], sum)); }
        return sum;
    }
    __device__ __forceinline__ void ReduceScatter(const float (&partial)[2][8], float (&own)[2][2], unsigned t)
    {
        const bool upper = (t & 2) != 0, odd = (t & 1) != 0;
        float kept[2][4];
#pragma unroll
        for (unsigned r = 0; r < 2; ++r) {
#pragma unroll
            for (unsigned i = 0; i < 4; ++i) {
                const float keep = upper ? partial[r][4 + i] : partial[r][i];
                const float give = upper ? partial[r][i] : partial[r][4 + i];
                kept[r][i] = keep + __shfl_xor_sync(0xFFFFFFFFu, give, 2);
            }
        }
#pragma unroll
        for (unsigned r = 0; r < 2; ++r) {
#pragma unroll
            for (unsigned i = 0; i < 2; ++i) {
                const float keep = odd ? kept[r][2 + i] : kept[r][i];
                const float give = odd ? kept[r][i] : kept[r][2 + i];
                own[r][i] = keep + __shfl_xor_sync(0xFFFFFFFFu, give, 1);
            }
        }
    }
    __device__ __forceinline__ unsigned Finish(const float (&own)[2], unsigned c)
    {
        return PackHalf2(own[0] + LowHalf(c), own[1] + HighHalf(c));
    }
}
'''


def analyze(ptx):
    """Block signatures used by a module (for generating the functions)."""
    signatures = set()
    for run in find_runs(ptx.replace('\r', '')):
        blocks = plan(run)
        if not blocks:
            continue
        mmas = [m for m, _ in run]
        source = sources(mmas)
        for block in blocks:
            signatures.add(block_signature(mmas, block, source))
    return signatures


def sources(mmas):
    writer, source = {}, []
    for i, m in enumerate(mmas):
        source.append({writer.get(r) for r in m.c}.pop())
        for r in m.d:
            writer[r] = i
    return source


def generate_cu(signatures):
    return HELPERS + '\n'.join(generate_function(s) for s in sorted(signatures)) + '\n'


def replace_runs(ptx, call):
    """Rewrites every groupable run as hoisted gap instructions plus one call per block.
    `call(function, arguments, results)` builds the PTX call. Returns (ptx, mma grouped)."""
    s = ptx.replace('\r', '')
    edits, grouped = [], 0
    for run in find_runs(s):
        blocks = plan(run)
        if not blocks:
            continue
        mmas = [m for m, _ in run]
        source = sources(mmas)
        lines = ['// PASCAL: grouped mma run'] + [g for _, gap in run for g in gap]
        for block in blocks:
            n_a, n_b, spec = signature = block_signature(mmas, block, source)
            a_regs, b_regs = [], []
            for i in block:
                if mmas[i].a not in a_regs:
                    a_regs.append(mmas[i].a)
                if mmas[i].b not in b_regs:
                    b_regs.append(mmas[i].b)
            arguments = [r for a in a_regs for r in a] + [r for b in b_regs for r in b]
            arguments += [r for k, i in enumerate(block) if spec[k][2] < 0 for r in mmas[i].c]
            results = [r for i in block for r in mmas[i].d]
            lines.append(call(function_name(signature), arguments, results, packed_result=True))
        edits.append((run[0][0].start, run[-1][0].end, '\n'.join(lines)))
        grouped += len(mmas)
    for start, end, text in reversed(edits):
        s = s[:start] + text + s[end:]
    return s, grouped
