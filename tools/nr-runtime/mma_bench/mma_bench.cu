// Research: cost of the FP8 mma stand-ins on their own. One warp per CTA and 16128 CTAs, like
// the swin_1h kernels; each warp runs 16 rounds of a 4x4 outer product (256 mma, one window),
// chaining C through the rounds. Variants: 16 per-mma calls, one 4x4 grouped call, two 2x4.
// Built with -rdc -maxrregcount=168 together with pascal_emulation.cu and the
// pascal_groups.generate_cu() functions (noinline), plus inline_defs.cuh: __forceinline__
// copies of the same code. GTX 1070 Ti, 2026-10-20: per-mma 19.5 ms, 4x4 group 93.7 / inlined
// 30.9, 2x4 group 28.4 / inlined 94.5, plain f32 FMA 5.3 (RTX 4080: 8.9, 6.1 / 4.0, 5.5 / 4.7, 1.1).
#include <cstdio>
#include <cuda_runtime.h>
#include "inline_defs.cuh"

extern "C" __device__ TrpPair trp_mma_e4m3(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
struct Ret44 { unsigned d[32]; };
struct Ret24 { unsigned d[16]; };
extern "C" __device__ Ret44 trp_g_d3da32b449e6(
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
extern "C" __device__ Ret24 trp_g_42beea0b847d(
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);

__device__ __forceinline__ void Load(const unsigned* in, unsigned (&a)[4][4], unsigned (&b)[4][2])
{
    const unsigned lane = threadIdx.x;
#pragma unroll
    for (int i = 0; i < 4; ++i) {
#pragma unroll
        for (int r = 0; r < 4; ++r) { a[i][r] = in[(i * 4 + r) * 32 + lane]; }
    }
#pragma unroll
    for (int j = 0; j < 4; ++j) {
#pragma unroll
        for (int r = 0; r < 2; ++r) { b[j][r] = in[(16 + j * 2 + r) * 32 + lane]; }
    }
}

__device__ __forceinline__ void Store(unsigned* out, const unsigned (&c)[16][2])
{
    unsigned x = 0;
#pragma unroll
    for (int k = 0; k < 16; ++k) { x ^= c[k][0] ^ (c[k][1] << 1); }
    out[blockIdx.x * 32 + threadIdx.x] = x;
}

__global__ void __launch_bounds__(32) Single(const unsigned* in, unsigned* out, int rounds)
{
    unsigned a[4][4], b[4][2], c[16][2] = {};
    Load(in, a, b);
    for (int round = 0; round < rounds; ++round) {
#pragma unroll
        for (int i = 0; i < 4; ++i) {
#pragma unroll
            for (int j = 0; j < 4; ++j) {
                const TrpPair d = trp_mma_e4m3(a[i][0], a[i][1], a[i][2], a[i][3], b[j][0], b[j][1], c[i * 4 + j][0], c[i * 4 + j][1]);
                c[i * 4 + j][0] = d.x; c[i * 4 + j][1] = d.y;
            }
        }
    }
    Store(out, c);
}

__global__ void __launch_bounds__(32) Group44(const unsigned* in, unsigned* out, int rounds)
{
    unsigned a[4][4], b[4][2], c[16][2] = {};
    Load(in, a, b);
    for (int round = 0; round < rounds; ++round) {
        const Ret44 d = trp_g_d3da32b449e6(
            a[0][0], a[0][1], a[0][2], a[0][3], a[1][0], a[1][1], a[1][2], a[1][3], a[2][0], a[2][1], a[2][2], a[2][3],
            a[3][0], a[3][1], a[3][2], a[3][3], b[0][0], b[0][1], b[1][0], b[1][1], b[2][0], b[2][1], b[3][0], b[3][1],
            c[0][0], c[0][1], c[1][0], c[1][1], c[2][0], c[2][1], c[3][0], c[3][1], c[4][0], c[4][1], c[5][0], c[5][1],
            c[6][0], c[6][1], c[7][0], c[7][1], c[8][0], c[8][1], c[9][0], c[9][1], c[10][0], c[10][1], c[11][0], c[11][1],
            c[12][0], c[12][1], c[13][0], c[13][1], c[14][0], c[14][1], c[15][0], c[15][1]);
#pragma unroll
        for (int k = 0; k < 16; ++k) { c[k][0] = d.d[2 * k]; c[k][1] = d.d[2 * k + 1]; }
    }
    Store(out, c);
}

__global__ void __launch_bounds__(32) Group24(const unsigned* in, unsigned* out, int rounds)
{
    unsigned a[4][4], b[4][2], c[16][2] = {};
    Load(in, a, b);
    for (int round = 0; round < rounds; ++round) {
#pragma unroll
        for (int h = 0; h < 2; ++h) {
            const int i = 2 * h, k0 = 8 * h;
            const Ret24 d = trp_g_42beea0b847d(
                a[i][0], a[i][1], a[i][2], a[i][3], a[i + 1][0], a[i + 1][1], a[i + 1][2], a[i + 1][3],
                b[0][0], b[0][1], b[1][0], b[1][1], b[2][0], b[2][1], b[3][0], b[3][1],
                c[k0][0], c[k0][1], c[k0 + 1][0], c[k0 + 1][1], c[k0 + 2][0], c[k0 + 2][1], c[k0 + 3][0], c[k0 + 3][1],
                c[k0 + 4][0], c[k0 + 4][1], c[k0 + 5][0], c[k0 + 5][1], c[k0 + 6][0], c[k0 + 6][1], c[k0 + 7][0], c[k0 + 7][1]);
#pragma unroll
            for (int k = 0; k < 8; ++k) { c[k0 + k][0] = d.d[2 * k]; c[k0 + k][1] = d.d[2 * k + 1]; }
        }
    }
    Store(out, c);
}

__global__ void __launch_bounds__(32) SingleInl(const unsigned* in, unsigned* out, int rounds)
{
    unsigned a[4][4], b[4][2], c[16][2] = {};
    Load(in, a, b);
    for (int round = 0; round < rounds; ++round) {
#pragma unroll
        for (int i = 0; i < 4; ++i) {
#pragma unroll
            for (int j = 0; j < 4; ++j) {
                const TrpPair d = trp_mma_e4m3_inl(a[i][0], a[i][1], a[i][2], a[i][3], b[j][0], b[j][1], c[i * 4 + j][0], c[i * 4 + j][1]);
                c[i * 4 + j][0] = d.x; c[i * 4 + j][1] = d.y;
            }
        }
    }
    Store(out, c);
}

__global__ void __launch_bounds__(32) Group44Inl(const unsigned* in, unsigned* out, int rounds)
{
    unsigned a[4][4], b[4][2], c[16][2] = {};
    Load(in, a, b);
    for (int round = 0; round < rounds; ++round) {
        const auto d = trp_g_d3da32b449e6_inl(
            a[0][0], a[0][1], a[0][2], a[0][3], a[1][0], a[1][1], a[1][2], a[1][3], a[2][0], a[2][1], a[2][2], a[2][3],
            a[3][0], a[3][1], a[3][2], a[3][3], b[0][0], b[0][1], b[1][0], b[1][1], b[2][0], b[2][1], b[3][0], b[3][1],
            c[0][0], c[0][1], c[1][0], c[1][1], c[2][0], c[2][1], c[3][0], c[3][1], c[4][0], c[4][1], c[5][0], c[5][1],
            c[6][0], c[6][1], c[7][0], c[7][1], c[8][0], c[8][1], c[9][0], c[9][1], c[10][0], c[10][1], c[11][0], c[11][1],
            c[12][0], c[12][1], c[13][0], c[13][1], c[14][0], c[14][1], c[15][0], c[15][1]);
#pragma unroll
        for (int k = 0; k < 16; ++k) { c[k][0] = d.d[2 * k]; c[k][1] = d.d[2 * k + 1]; }
    }
    Store(out, c);
}

__global__ void __launch_bounds__(32) Group24Inl(const unsigned* in, unsigned* out, int rounds)
{
    unsigned a[4][4], b[4][2], c[16][2] = {};
    Load(in, a, b);
    for (int round = 0; round < rounds; ++round) {
#pragma unroll
        for (int h = 0; h < 2; ++h) {
            const int i = 2 * h, k0 = 8 * h;
            const auto d = trp_g_42beea0b847d_inl(
                a[i][0], a[i][1], a[i][2], a[i][3], a[i + 1][0], a[i + 1][1], a[i + 1][2], a[i + 1][3],
                b[0][0], b[0][1], b[1][0], b[1][1], b[2][0], b[2][1], b[3][0], b[3][1],
                c[k0][0], c[k0][1], c[k0 + 1][0], c[k0 + 1][1], c[k0 + 2][0], c[k0 + 2][1], c[k0 + 3][0], c[k0 + 3][1],
                c[k0 + 4][0], c[k0 + 4][1], c[k0 + 5][0], c[k0 + 5][1], c[k0 + 6][0], c[k0 + 6][1], c[k0 + 7][0], c[k0 + 7][1]);
#pragma unroll
            for (int k = 0; k < 8; ++k) { c[k0 + k][0] = d.d[2 * k]; c[k0 + k][1] = d.d[2 * k + 1]; }
        }
    }
    Store(out, c);
}

// Same arithmetic volume in plain f32 FMA with operands in registers: the floor.
__global__ void __launch_bounds__(32) FmaFloor(const unsigned* in, unsigned* out, int rounds)
{
    float acc[64] = {};
    float a[8], b[8];
#pragma unroll
    for (int i = 0; i < 8; ++i) { a[i] = __uint_as_float(in[i * 32 + threadIdx.x] & 0x3F7FFFFFu); b[i] = __uint_as_float(in[(8 + i) * 32 + threadIdx.x] & 0x3F7FFFFFu); }
    for (int round = 0; round < rounds * 32; ++round) {   // 2048 FMA per lane per round = 16 mma
#pragma unroll
        for (int i = 0; i < 8; ++i) {
#pragma unroll
            for (int j = 0; j < 8; ++j) { acc[i * 8 + j] = fmaf(a[i], b[j], acc[i * 8 + j]); }
        }
        a[round & 7] += 1e-7f;
    }
    float x = 0;
    for (int i = 0; i < 64; ++i) { x += acc[i]; }
    out[blockIdx.x * 32 + threadIdx.x] = __float_as_uint(x);
}

int main()
{
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    const int ctas = 16128, rounds = 16;
    unsigned *in, *out;
    cudaMalloc(&in, 64 * 32 * sizeof(unsigned));
    cudaMalloc(&out, ctas * 32 * sizeof(unsigned));
    unsigned host[64 * 32];
    for (int i = 0; i < 64 * 32; ++i) { host[i] = (i * 2654435761u) & 0x3B3B3B3Bu; }
    cudaMemcpy(in, host, sizeof(host), cudaMemcpyHostToDevice);
    cudaEvent_t start, stop;
    cudaEventCreate(&start); cudaEventCreate(&stop);
    struct { const char* name; void (*kernel)(const unsigned*, unsigned*, int); } variants[] = {
        {"16 x trp_mma_e4m3", Single}, {"1 x group 4x4", Group44}, {"2 x group 2x4", Group24}, {"16 x inlined per-mma", SingleInl}, {"1 x 4x4 inlined", Group44Inl}, {"2 x 2x4 inlined", Group24Inl}, {"plain f32 FMA floor", FmaFloor}};
    std::printf("%s, %d SMs, %d MHz: %d CTAs x 1 warp, %d rounds of 16 mma (one window each)\n",
                prop.name, prop.multiProcessorCount, prop.clockRate / 1000, ctas, rounds);
    for (auto& v : variants) {
        v.kernel<<<ctas, 32>>>(in, out, rounds);
        cudaDeviceSynchronize();
        float best = 1e30f;
        for (int rep = 0; rep < 5; ++rep) {
            cudaEventRecord(start);
            v.kernel<<<ctas, 32>>>(in, out, rounds);
            cudaEventRecord(stop);
            cudaEventSynchronize(stop);
            float ms = 0;
            cudaEventElapsedTime(&ms, start, stop);
            best = ms < best ? ms : best;
        }
        cudaFuncAttributes attr;
        cudaFuncGetAttributes(&attr, v.kernel);
        std::printf("  %-22s %8.3f ms  (%d regs, %zu B local)  %s\n", v.name, best, attr.numRegs, attr.localSizeBytes,
                    cudaGetErrorString(cudaGetLastError()));
    }
    return 0;
}
