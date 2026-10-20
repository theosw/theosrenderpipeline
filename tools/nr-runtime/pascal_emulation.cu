// Pascal (sm_61) stand-ins for the sm_75+ instructions in NVIDIA NR 310.9.16's network
// kernels. pascal_lowering.py splices the PTX of these functions into each module and
// replaces every use of the original instruction with a call. Exact for every input
// except NaN encodings, which these kernels never produce. Build without fast math:
// the FP8 decode relies on f32 subnormals surviving a multiply.
#include <cuda_fp16.h>

namespace
{
    __device__ __forceinline__ unsigned LaneId()
    {
        unsigned lane;
        asm volatile("mov.u32 %0, %%laneid;" : "=r"(lane));
        return lane;
    }

    // e4m3 byte -> f32, exactly: move the fields into f32 position and rescale the
    // exponent bias (7 -> 127) by 2^120. Subnormals stay exact.
    __device__ __forceinline__ float E4M3(unsigned bits)
    {
        return __uint_as_float(((bits & 0x80u) << 24) | ((bits & 0x7Fu) << 20)) * 1.329227995784916e36f;
    }

    // f32 -> e4m3 byte with round-to-nearest-even and saturation to +-448 (.rn.satfinite).
    __device__ __forceinline__ unsigned EncodeE4M3(float value)
    {
        const unsigned sign = (__float_as_uint(value) >> 24) & 0x80u;
        if (value != value) { return sign | 0x7Fu; }
        const float magnitude = fminf(fabsf(value), 448.0f);
        unsigned code;
        if (magnitude < 0.015625f) {
            code = static_cast<unsigned>(__float2int_rn(magnitude * 512.0f));  // Subnormal grid 2^-9.
        } else {
            unsigned bits = __float_as_uint(magnitude);
            bits += 0x7FFFFu + ((bits >> 20) & 1u);
            code = (bits >> 20) - (120u << 3);
        }
        return sign | code;
    }

    __device__ __forceinline__ unsigned PackHalf2(float low, float high)
    {
        return static_cast<unsigned>(__half_as_ushort(__float2half_rn(low))) |
            (static_cast<unsigned>(__half_as_ushort(__float2half_rn(high))) << 16);
    }

    __device__ __forceinline__ float LowHalf(unsigned value) { return __half2float(__ushort_as_half(static_cast<unsigned short>(value))); }
    __device__ __forceinline__ float HighHalf(unsigned value) { return __half2float(__ushort_as_half(static_cast<unsigned short>(value >> 16))); }
}

struct TrpPair { unsigned x, y; };

// mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16 {d0,d1}, {a0..a3}, {b0,b1}, {c0,c1}.
// Lane (g = lane/4, t = lane%4) owns D rows g, g+8 and columns 2t, 2t+1. A row g lives in
// a0 (k 0-15) and a2 (k 16-31) of lanes 4g..4g+3, each holding 4 consecutive k; B column n
// lives in b0/b1 of lanes 4n..4n+3 the same way.
//
// Each lane already holds A rows g and g+8 for its 8 k values (k = 4t..4t+3 in a0/a1 and
// 4t+16..4t+19 in a2/a3) and lane (n, t) holds B column n for the same k. So each lane
// decodes its own A once, fetches B column c for its k from lane 4c+t (16 shuffles),
// forms partial sums for all 16 outputs of its rows, and the four lanes of a row group
// then reduce-scatter so lane t keeps columns 2t, 2t+1 (12 shuffles).
extern "C" __device__ __noinline__ TrpPair trp_mma_e4m3(
    unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned b0, unsigned b1, unsigned c0, unsigned c1)
{
    const unsigned lane = LaneId(), t = lane & 3;
    float low[8], high[8];  // A row g and g+8 at this lane's 8 k.
#pragma unroll
    for (unsigned q = 0; q < 4; ++q) {
        low[q] = E4M3(a0 >> (8 * q)); low[4 + q] = E4M3(a2 >> (8 * q));
        high[q] = E4M3(a1 >> (8 * q)); high[4 + q] = E4M3(a3 >> (8 * q));
    }
    float partial[2][8];  // [row g / g+8][column 0..7], summed over this lane's k.
#pragma unroll
    for (unsigned c = 0; c < 8; ++c) {
        const unsigned source = (c << 2) | t;
        const unsigned k0 = __shfl_sync(0xFFFFFFFFu, b0, source), k1 = __shfl_sync(0xFFFFFFFFu, b1, source);
        float sumLow = 0.0f, sumHigh = 0.0f;
#pragma unroll
        for (unsigned q = 0; q < 4; ++q) {
            const float bk0 = E4M3(k0 >> (8 * q)), bk1 = E4M3(k1 >> (8 * q));
            sumLow = fmaf(low[q], bk0, fmaf(low[4 + q], bk1, sumLow));
            sumHigh = fmaf(high[q], bk0, fmaf(high[4 + q], bk1, sumHigh));
        }
        partial[0][c] = sumLow; partial[1][c] = sumHigh;
    }
    // Reduce-scatter over the row group's lanes: after xor 2 a lane keeps the 4 columns of
    // owners {t, t^1}; after xor 1 it keeps its own 2 columns. Selects, not indexing, keep
    // everything in registers.
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
    float own[2][2];
#pragma unroll
    for (unsigned r = 0; r < 2; ++r) {
#pragma unroll
        for (unsigned i = 0; i < 2; ++i) {
            const float keep = odd ? kept[r][2 + i] : kept[r][i];
            const float give = odd ? kept[r][i] : kept[r][2 + i];
            own[r][i] = keep + __shfl_xor_sync(0xFFFFFFFFu, give, 1);
        }
    }
    return {PackHalf2(own[0][0] + LowHalf(c0), own[0][1] + HighHalf(c0)),
            PackHalf2(own[1][0] + LowHalf(c1), own[1][1] + HighHalf(c1))};
}

// mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16. A row g: a0 (k 2t,2t+1) and a2
// (k 2t+8,2t+9) of lanes 4g..4g+3; row g+8 in a1/a3. B column n: b0/b1 of lanes 4n..4n+3.
extern "C" __device__ __noinline__ TrpPair trp_mma_f16(
    unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned b0, unsigned b1, unsigned c0, unsigned c1)
{
    const unsigned lane = LaneId(), g = lane >> 2, t = lane & 3;
    float d00 = LowHalf(c0), d01 = HighHalf(c0), d10 = LowHalf(c1), d11 = HighHalf(c1);
    for (unsigned j = 0; j < 4; ++j) {
        const unsigned sourceA = (g << 2) | j, sourceB0 = ((2 * t) << 2) | j, sourceB1 = ((2 * t + 1) << 2) | j;
        const unsigned rowLow0 = __shfl_sync(0xFFFFFFFFu, a0, sourceA), rowLow1 = __shfl_sync(0xFFFFFFFFu, a2, sourceA);
        const unsigned rowHigh0 = __shfl_sync(0xFFFFFFFFu, a1, sourceA), rowHigh1 = __shfl_sync(0xFFFFFFFFu, a3, sourceA);
        const unsigned col0k0 = __shfl_sync(0xFFFFFFFFu, b0, sourceB0), col0k1 = __shfl_sync(0xFFFFFFFFu, b1, sourceB0);
        const unsigned col1k0 = __shfl_sync(0xFFFFFFFFu, b0, sourceB1), col1k1 = __shfl_sync(0xFFFFFFFFu, b1, sourceB1);
        const float al[4]{LowHalf(rowLow0), HighHalf(rowLow0), LowHalf(rowLow1), HighHalf(rowLow1)};
        const float ah[4]{LowHalf(rowHigh0), HighHalf(rowHigh0), LowHalf(rowHigh1), HighHalf(rowHigh1)};
        const float b0k[4]{LowHalf(col0k0), HighHalf(col0k0), LowHalf(col0k1), HighHalf(col0k1)};
        const float b1k[4]{LowHalf(col1k0), HighHalf(col1k0), LowHalf(col1k1), HighHalf(col1k1)};
#pragma unroll
        for (int q = 0; q < 4; ++q) {
            d00 = fmaf(al[q], b0k[q], d00);
            d01 = fmaf(al[q], b1k[q], d01);
            d10 = fmaf(ah[q], b0k[q], d10);
            d11 = fmaf(ah[q], b1k[q], d11);
        }
    }
    return {PackHalf2(d00, d01), PackHalf2(d10, d11)};
}

// cvt.rn.satfinite.e4m3x2.f16x2 d, a: low half -> low byte.
extern "C" __device__ __noinline__ unsigned trp_cvt_e4m3x2_f16x2(unsigned a)
{
    return EncodeE4M3(LowHalf(a)) | (EncodeE4M3(HighHalf(a)) << 8);
}

// cvt.rn.f16x2.e4m3x2 d, a: low byte -> low half. Exact (both values fit f16).
extern "C" __device__ __noinline__ unsigned trp_cvt_f16x2_e4m3x2(unsigned a)
{
    return PackHalf2(E4M3(a), E4M3(a >> 8));
}

// movmatrix.sync.trans.aligned.m8n8.b16 d, a. Lane l holds row l/4, columns 2(l%4) and
// 2(l%4)+1; the transpose's element (r, c) is the source's (c, r).
extern "C" __device__ __noinline__ unsigned trp_movmatrix_trans(unsigned a)
{
    const unsigned lane = LaneId(), row = lane >> 2, column = 2 * (lane & 3);
    const unsigned v0 = __shfl_sync(0xFFFFFFFFu, a, 4 * column + (row >> 1));
    const unsigned v1 = __shfl_sync(0xFFFFFFFFu, a, 4 * (column + 1) + (row >> 1));
    const unsigned shift = (row & 1) * 16;
    return ((v0 >> shift) & 0xFFFFu) | (((v1 >> shift) & 0xFFFFu) << 16);
}

// min.f16x2 / max.f16x2: IEEE minNum/maxNum per half, -0 below +0, like the sm_80 instruction.
extern "C" __device__ __noinline__ unsigned trp_min_f16x2(unsigned a, unsigned b)
{
    return PackHalf2(fminf(LowHalf(a), LowHalf(b)), fminf(HighHalf(a), HighHalf(b)));
}

extern "C" __device__ __noinline__ unsigned trp_max_f16x2(unsigned a, unsigned b)
{
    return PackHalf2(fmaxf(LowHalf(a), LowHalf(b)), fmaxf(HighHalf(a), HighHalf(b)));
}
