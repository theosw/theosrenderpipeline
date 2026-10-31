// Research: native Pascal (sm_61) replacements for NR 310.9.16 ViT kernels. Same entry
// names, parameter blocks, data layouts and flag protocol as NVIDIA's kernels; FP8 is
// decoded once per CTA into shared memory and multiplied with f32 FMA.
//
// Layouts (verified against NVIDIA's kernels on an RTX 4080 Super, bench/layouts.py):
//   activations: 16-token blocks of tokens*channels bytes; per 32-channel slice 512 bytes
//     in mma A-fragment order: lane 4g+t holds rows g / g+8, stored k 4t..4t+3 / +16.
//   kernel outputs: written from accumulators, so stored k 4t+i of a 16-channel half
//     holds channel 2t + (i&1) + 8(i>>1).
//   weights: 32-k slices of N*32 bytes; per 64 output channels 2048 bytes; load j of lane
//     4g+t holds n-tiles 2j, 2j+1 (8 columns each) as b0 (k 4t..4t+3), b1 (k 4t+16..).
#include <cuda_fp16.h>

namespace
{
    __device__ __forceinline__ float E4M3(unsigned bits)
    {
        return __uint_as_float(((bits & 0x80u) << 24) | ((bits & 0x7Fu) << 20)) * 1.329227995784916e36f;
    }

    __device__ __forceinline__ unsigned EncodeE4M3(float value)
    {
        const unsigned sign = (__float_as_uint(value) >> 24) & 0x80u;
        if (value != value) { return sign | 0x7Fu; }
        const float magnitude = fminf(fabsf(value), 448.0f);
        unsigned code;
        if (magnitude < 0.015625f) {
            code = static_cast<unsigned>(__float2int_rn(magnitude * 512.0f));
        } else {
            unsigned bits = __float_as_uint(magnitude);
            bits += 0x7FFFFu + ((bits >> 20) & 1u);
            code = (bits >> 20) - (120u << 3);
        }
        return sign | code;
    }

    __device__ __forceinline__ float Half(float value) { return __half2float(__float2half_rn(value)); }

    // NVIDIA's MpCubicSiluActivation in f16 arithmetic (each step rounded to f16):
    // y * 1.7890625 * (1/2 + c/4 - c|c|/32), c = clamp(y, -4, 4).
    __device__ __forceinline__ float CubicSilu(float y)
    {
        const float c = fminf(fmaxf(y, -4.0f), 4.0f);
        const float p = Half(fmaf(-0.055908203125f, fabsf(c), 0.447265625f));
        const float t = Half(fmaf(c, p, 0.89453125f));
        return Half(y * t);
    }

    struct __align__(8) Conv1x1Params
    {
        const unsigned char* input;    // +0
        const void* unused8;           // +8
        unsigned char* output;         // +16
        const unsigned char* weights;  // +24
        const void* unused32;          // +32
        const void* unused40;          // +40
        const int* waitFlags;          // +48, chained: producer tile flags
        int* signalFlags;              // +56, chained/publish: this kernel's tile flags
        int height, width;             // +64
    };
    static_assert(sizeof(Conv1x1Params) == 72, "parameter block size");

    // One CTA (4 warps) computes 128 tokens x 128 output channels of an FP8 1x1 conv over
    // K input channels of a C-channel input (`input` and `weights` already offset to the
    // first of them). Warp w: token blocks 4(w&1)..+3 of the tile, output slices
    // 2(w>>1), +1. Lane 4g+t: rows g, g+8 of each block; stored positions 4t..4t+3 and
    // 16+4t.. of each slice (16 channels). Shared: A[k][128 tokens], B[k][128 channels],
    // reordered so each thread's 8 tokens / 16 channels are contiguous.
    template <int K, int C, int N>
    __device__ __forceinline__ void Conv1x1Tile(const unsigned char* input, const unsigned char* weights,
                                                float (&acc)[8][16], int tokenTile, int channelTile, int blocks16)
    {
        __shared__ __align__(16) float As[32][128];
        __shared__ __align__(16) float Bs[32][128];
        const int lane = threadIdx.x, warp = threadIdx.y, thread = warp * 32 + lane;
        const int g = lane >> 2, t = lane & 3, wr = warp & 1, wc = warp >> 1;
#pragma unroll
        for (int i = 0; i < 8; ++i) {
#pragma unroll
            for (int j = 0; j < 16; ++j) { acc[i][j] = 0.0f; }
        }
        for (int slice = 0; slice < K / 32; ++slice) {
            __syncthreads();
            {   // A: 8 token blocks x 512 bytes; thread loads 32 bytes of block thread/16.
                const int block = thread >> 4, part = thread & 15;
                const int globalBlock = tokenTile * 8 + block;
                uint4 words[2] = {};
                if (globalBlock < blocks16) {
                    const uint4* source = reinterpret_cast<const uint4*>(
                        input + static_cast<size_t>(globalBlock) * 16 * C + slice * 512 + part * 32);
                    words[0] = source[0]; words[1] = source[1];
                }
                const unsigned* w = reinterpret_cast<const unsigned*>(words);
#pragma unroll
                for (int word = 0; word < 8; ++word) {
                    const int offset = part * 32 + word * 4;       // byte offset in the 512-byte slice
                    const int fragLane = offset >> 4, reg = (offset >> 2) & 3;
                    const int row = (fragLane >> 2) + 8 * (reg & 1);
                    const int kBase = 4 * (fragLane & 3) + 16 * (reg >> 1);
                    const int slot = (((block >> 2) * 8 + (row & 7)) * 8) + (block & 3) * 2 + (row >> 3);
#pragma unroll
                    for (int byte = 0; byte < 4; ++byte) { As[kBase + byte][slot] = E4M3(w[word] >> (8 * byte)); }
                }
            }
            {   // B: 128 channels x 32 k = 4096 contiguous bytes; thread loads 32 bytes.
                const uint4* source = reinterpret_cast<const uint4*>(
                    weights + static_cast<size_t>(slice) * N * 32 +
                    static_cast<size_t>(channelTile) * 128 * 32 + thread * 32);
                const uint4 words[2] = {source[0], source[1]};
                const unsigned* w = reinterpret_cast<const unsigned*>(words);
#pragma unroll
                for (int word = 0; word < 8; ++word) {
                    const int offset = thread * 32 + word * 4;      // byte offset in the 4096 bytes
                    const int group = offset >> 11, rest = offset & 2047;
                    const int load = rest >> 9, fragLane = (rest >> 4) & 31, q = (rest >> 2) & 3;
                    const int nTile = 2 * load + (q >> 1), kBase = 4 * (fragLane & 3) + 16 * (q & 1);
                    const int n = group * 64 + nTile * 8 + (fragLane >> 2);   // channel within the tile
                    const int sl = n >> 5, h2 = (n >> 4) & 1, hi = (n >> 3) & 1, tt = (n >> 1) & 3, lo = n & 1;
                    const int slot = ((sl >> 1) * 4 + tt) * 16 + (sl & 1) * 8 + h2 * 4 + hi * 2 + lo;
#pragma unroll
                    for (int byte = 0; byte < 4; ++byte) { Bs[kBase + byte][slot] = E4M3(w[word] >> (8 * byte)); }
                }
            }
            __syncthreads();
            const float4* aRow = reinterpret_cast<const float4*>(&As[0][(wr * 8 + g) * 8]);
            const float4* bRow = reinterpret_cast<const float4*>(&Bs[0][(wc * 4 + t) * 16]);
#pragma unroll 4
            for (int k = 0; k < 32; ++k) {
                const float4 a0 = aRow[k * 32], a1 = aRow[k * 32 + 1];
                const float4 b0 = bRow[k * 32], b1 = bRow[k * 32 + 1], b2 = bRow[k * 32 + 2], b3 = bRow[k * 32 + 3];
                const float a[8] = {a0.x, a0.y, a0.z, a0.w, a1.x, a1.y, a1.z, a1.w};
                const float b[16] = {b0.x, b0.y, b0.z, b0.w, b1.x, b1.y, b1.z, b1.w,
                                     b2.x, b2.y, b2.z, b2.w, b3.x, b3.y, b3.z, b3.w};
#pragma unroll
                for (int i = 0; i < 8; ++i) {
#pragma unroll
                    for (int j = 0; j < 16; ++j) { acc[i][j] = fmaf(a[i], b[j], acc[i][j]); }
                }
            }
        }
    }

    // Writes acc (after `epilogue`) as FP8 in NVIDIA's accumulator output layout.
    template <int N, class Epilogue>
    __device__ __forceinline__ void StoreTile(const Conv1x1Params& p, const float (&acc)[8][16], int tokenTile,
                                              int channelTile, int blocks16, Epilogue epilogue)
    {
        const int lane = threadIdx.x, warp = threadIdx.y, wr = warp & 1, wc = warp >> 1;
#pragma unroll
        for (int bi = 0; bi < 4; ++bi) {
            const int globalBlock = tokenTile * 8 + wr * 4 + bi;
            if (globalBlock >= blocks16) { continue; }
#pragma unroll
            for (int sl = 0; sl < 2; ++sl) {
                unsigned words[4];
#pragma unroll
                for (int r = 0; r < 4; ++r) {   // a0: row g idx 0-3, a1: row g+8 idx 0-3, a2/a3: idx 4-7
                    const int row = bi * 2 + (r & 1), base = sl * 8 + 4 * (r >> 1);
                    words[r] = EncodeE4M3(epilogue(acc[row][base])) | (EncodeE4M3(epilogue(acc[row][base + 1])) << 8) |
                               (EncodeE4M3(epilogue(acc[row][base + 2])) << 16) | (EncodeE4M3(epilogue(acc[row][base + 3])) << 24);
                }
                const int slice = channelTile * 4 + wc * 2 + sl;
                *reinterpret_cast<uint4*>(p.output + static_cast<size_t>(globalBlock) * 16 * N + slice * 512 + lane * 16) =
                    make_uint4(words[0], words[1], words[2], words[3]);
            }
        }
    }

    __device__ __forceinline__ void WaitFlag(const int* flag)
    {
        while (*reinterpret_cast<const volatile int*>(flag) < 0) {}
    }

    template <bool Wait, bool Signal>
    __device__ __forceinline__ void FfnExpand(const Conv1x1Params& p)
    {
        constexpr int K = 1024, N = 4096;
        const int tokens = p.height * p.width;
        const int tiles = (tokens - 1) / 128 + 1;
        const int channelTile = blockIdx.x / tiles, tokenTile = blockIdx.x % tiles;
        const int blocks16 = (((tokens - 1) + 32) & ~31) / 16;
        if (Wait) {   // Producer: 1024 channels in 8 tiles of 128 per 128-token tile.
            if (threadIdx.x == 0 && threadIdx.y == 0) {
                const int first = (tokenTile * 128) >> 7, last = min(((tokenTile * 128 + 127) >> 7) + 1, tiles);
                for (int tile = first; tile < last; ++tile) {
                    for (int c = 0; c < K / 128; ++c) { WaitFlag(p.waitFlags + tile * 8 + c); }
                }
                __threadfence();
            }
            __syncthreads();
        }
        float acc[8][16];
        Conv1x1Tile<K, K, N>(p.input, p.weights, acc, tokenTile, channelTile, blocks16);
        StoreTile<N>(p, acc, tokenTile, channelTile, blocks16, [](float y) { return CubicSilu(Half(y)); });
        if (Signal) {
            __threadfence();
            __syncthreads();
            if (threadIdx.x == 0 && threadIdx.y == 0) {
                *reinterpret_cast<volatile int*>(p.signalFlags + tokenTile * 32 + channelTile) = 0;
            }
        }
    }

    struct __align__(8) ContractParams
    {
        const unsigned char* input;     // +0, 4096 channels
        const unsigned char* residual;  // +8, 1024 channels, same layout as the output
        unsigned char* output;          // +16
        const unsigned char* weights;   // +24, then f16 gamma[1024] (residual scale)
        int* sliceFlags;                // +32, per tile: last K quarter (z) folded in
        __half* partial;                // +40, f16 running sum, one per output byte
        const int* waitFlags;           // +48, chained: ffn_expand tile flags
        int* signalFlags;               // +56, chained: this kernel's tile flags
        int height, width;              // +64
    };
    static_assert(sizeof(ContractParams) == 72, "parameter block size");

    // Split K over blockIdx.z (quarters of 1024 input channels), folded in z order as NVIDIA
    // does: z=0 stores residual + its sum as f16, z=1 and 2 add theirs in f16, z=3 adds its
    // own and writes FP8. sliceFlags[tile] = z once quarter z is in.
    template <bool Wait, bool Signal>
    __device__ __forceinline__ void FfnContract(const ContractParams& p)
    {
        constexpr int K = 1024, C = 4096, N = 1024;
        const int tokens = p.height * p.width;
        const int tiles = (tokens - 1) / 128 + 1;
        const int channelTile = blockIdx.x / tiles, tokenTile = blockIdx.x % tiles;
        const int blocks16 = (((tokens - 1) + 32) & ~31) / 16;
        const int z = blockIdx.z, lane = threadIdx.x, warp = threadIdx.y, wr = warp & 1, wc = warp >> 1;
        const bool leader = threadIdx.x == 0 && threadIdx.y == 0;
        if (Wait) {   // NVIDIA waits on the first ffn_expand channel tile of this quarter; we
                      // read all 8 at once, so wait on each.
            if (leader) {
                for (int y = 0; y < 4; y += 2) {
                    const int first = tokenTile * 128 + y * 32;
                    const int last = min(((first + 32) >> 7) + 1, tiles);
                    for (int tile = first >> 7; tile < last; ++tile) {
                        for (int c = 0; c < 8; ++c) { WaitFlag(p.waitFlags + tile * 32 + z * 8 + c); }
                    }
                }
                __threadfence();
            }
            __syncthreads();
        }
        float acc[8][16];
        Conv1x1Tile<K, C, N>(p.input + z * (K / 32) * 512, p.weights + static_cast<size_t>(z) * K * N, acc,
                             tokenTile, channelTile, blocks16);
        const int flag = tokenTile * 8 + channelTile;
        if (z > 0) {
            if (leader) {
                while (*reinterpret_cast<const volatile int*>(p.sliceFlags + flag) < z - 1) {}
                __threadfence();
            }
            __syncthreads();
        }
#pragma unroll
        for (int bi = 0; bi < 4; ++bi) {
            const int globalBlock = tokenTile * 8 + wr * 4 + bi;
            if (globalBlock >= blocks16) { continue; }
#pragma unroll
            for (int sl = 0; sl < 2; ++sl) {
                const size_t base = static_cast<size_t>(globalBlock) * 16 * N + (channelTile * 4 + wc * 2 + sl) * 512 + lane * 16;
                float v[16];
#pragma unroll
                for (int q = 0; q < 16; ++q) {   // lane byte q: word q/4 as in StoreTile
                    v[q] = acc[bi * 2 + ((q >> 2) & 1)][sl * 8 + 4 * (q >> 3) + (q & 3)];
                }
                uint4* partial = reinterpret_cast<uint4*>(p.partial + base);
                if (z == 0) {   // + residual * gamma, the f16 per-channel scale after the weights
                    const uint4 r = *reinterpret_cast<const uint4*>(p.residual + base);
                    const unsigned rw[4] = {r.x, r.y, r.z, r.w};
                    const unsigned* gamma = reinterpret_cast<const unsigned*>(p.weights + C * N) +
                                            (channelTile * 4 + wc * 2 + sl) * 16 + (lane & 3);
#pragma unroll
                    for (int q = 0; q < 16; ++q) {   // channel 16(q>>3) + 8((q>>1)&1) + 2t + (q&1) of the slice
                        const unsigned pair = __ldg(gamma + 8 * (q >> 3) + 4 * ((q >> 1) & 1));
                        const float g = __half2float(__ushort_as_half(static_cast<unsigned short>(pair >> (16 * (q & 1)))));
                        v[q] = Half(v[q] + Half(E4M3(rw[q >> 2] >> (8 * (q & 3))) * g));
                    }
                } else {
                    const uint4 s0 = __ldcg(partial), s1 = __ldcg(partial + 1);
                    const unsigned sw[8] = {s0.x, s0.y, s0.z, s0.w, s1.x, s1.y, s1.z, s1.w};
#pragma unroll
                    for (int q = 0; q < 16; ++q) {
                        const __half h = __ushort_as_half(static_cast<unsigned short>(sw[q >> 1] >> (16 * (q & 1))));
                        v[q] = Half(__half2float(h) + Half(v[q]));
                    }
                }
                if (z < 3) {
                    unsigned hw[8];
#pragma unroll
                    for (int i = 0; i < 8; ++i) {
                        hw[i] = __half_as_ushort(__float2half_rn(v[2 * i])) |
                                (static_cast<unsigned>(__half_as_ushort(__float2half_rn(v[2 * i + 1]))) << 16);
                    }
                    __stcg(partial, make_uint4(hw[0], hw[1], hw[2], hw[3]));
                    __stcg(partial + 1, make_uint4(hw[4], hw[5], hw[6], hw[7]));
                } else {
                    unsigned ow[4];
#pragma unroll
                    for (int i = 0; i < 4; ++i) {
                        ow[i] = EncodeE4M3(v[4 * i]) | (EncodeE4M3(v[4 * i + 1]) << 8) |
                                (EncodeE4M3(v[4 * i + 2]) << 16) | (EncodeE4M3(v[4 * i + 3]) << 24);
                    }
                    *reinterpret_cast<uint4*>(p.output + base) = make_uint4(ow[0], ow[1], ow[2], ow[3]);
                }
            }
        }
        __threadfence();
        __syncthreads();
        if (leader) {
            *reinterpret_cast<volatile int*>(p.sliceFlags + flag) = z;
            if (Signal && z == 3) { *reinterpret_cast<volatile int*>(p.signalFlags + flag) = 0; }
        }
    }

    struct __align__(8) QkvParams
    {
        const unsigned char* input;    // +0, 1024 channels
        unsigned char* q;              // +8, 32 heads of 32 channels, laid out as 1024 channels
        unsigned char* k;              // +16
        unsigned char* v;              // +24
        const unsigned char* weights;  // +32: f32 Q scale per head [32], then FP8 weights (N = 3072)
        int* pairFlags;                // +40, per (token tile, head pair); NVIDIA's split-K flags
        const __half* partial;         // +48, NVIDIA's split-K f16 scratch (unused here)
        const int* waitFlags;          // +56, chained: ffn_contract tile flags
        int* signalFlags;              // +64, chained: this kernel's (token tile, head pair) flags
        int height, width;             // +72
    };
    static_assert(sizeof(QkvParams) == 80, "parameter block size");

    // CTA x: token tile x % tiles, head pair x / tiles. Weight columns 96 * head + [Q 32 |
    // K 32 | V 32]. Q and K are divided by their L2 norm over the head (clamped at the f16
    // eps NVIDIA uses), Q then scaled by sqrt(32) and its head's scale, all in f16 steps.
    // NVIDIA splits K over z and folds the halves in f16 through `partial`; here z picks the
    // head of the pair and each CTA reads all of K. The second CTA of a pair to finish signals.
    // Warp w: token blocks 2w, 2w+1 of the tile; lane 4g+t: rows g, g+8, stored positions
    // 4t..4t+3 and 16+4t.. of the Q, K and V slices.
    template <bool Wait, bool Signal>
    __device__ __forceinline__ void Qkv(const QkvParams& p)
    {
        constexpr int K = 1024, N = 3072, Cols = 96;
        __shared__ __align__(16) float As[32][128];
        __shared__ __align__(16) float Bs[32][Cols];
        const int tokens = p.height * p.width;
        const int tiles = (tokens - 1) / 128 + 1;
        const int pair = blockIdx.x / tiles, tokenTile = blockIdx.x % tiles;
        const int blocks16 = (((tokens - 1) + 32) & ~31) / 16;
        const int lane = threadIdx.x, warp = threadIdx.y, thread = warp * 32 + lane, g = lane >> 2, t = lane & 3;
        if (Wait) {
            if (thread == 0) {
                for (int c = 0; c < K / 128; ++c) { WaitFlag(p.waitFlags + tokenTile * 8 + c); }
                __threadfence();
            }
            __syncthreads();
        }
        const float eps = Half(__int_as_float(948045311)), root32 = Half(sqrtf(32.0f));
        for (int h = blockIdx.z; h < 2; h += gridDim.z) {
            const int head = pair * 2 + h;
            const unsigned char* weights = p.weights + 128 + static_cast<size_t>(head) * Cols * 32;
            float acc[4][24];
#pragma unroll
            for (int i = 0; i < 4; ++i) {
#pragma unroll
                for (int j = 0; j < 24; ++j) { acc[i][j] = 0.0f; }
            }
            for (int slice = 0; slice < K / 32; ++slice) {
                __syncthreads();
                {   // A: 8 token blocks x 512 bytes; thread loads 32 bytes of block thread/16.
                    const int block = thread >> 4, part = thread & 15;
                    const int globalBlock = tokenTile * 8 + block;
                    uint4 words[2] = {};
                    if (globalBlock < blocks16) {
                        const uint4* source = reinterpret_cast<const uint4*>(
                            p.input + static_cast<size_t>(globalBlock) * 16 * K + slice * 512 + part * 32);
                        words[0] = source[0]; words[1] = source[1];
                    }
                    const unsigned* w = reinterpret_cast<const unsigned*>(words);
#pragma unroll
                    for (int word = 0; word < 8; ++word) {
                        const int offset = part * 32 + word * 4;
                        const int fragLane = offset >> 4, reg = (offset >> 2) & 3;
                        const int row = (fragLane >> 2) + 8 * (reg & 1);
                        const int kBase = 4 * (fragLane & 3) + 16 * (reg >> 1);
                        const int slot = ((block >> 1) * 8 + (row & 7)) * 4 + (block & 1) * 2 + (row >> 3);
#pragma unroll
                        for (int byte = 0; byte < 4; ++byte) { As[kBase + byte][slot] = E4M3(w[word] >> (8 * byte)); }
                    }
                }
                if (thread < Cols) {   // B: 96 columns x 32 k = 3072 contiguous bytes.
                    const uint4* source = reinterpret_cast<const uint4*>(weights + static_cast<size_t>(slice) * N * 32 + thread * 32);
                    const uint4 words[2] = {source[0], source[1]};
                    const unsigned* w = reinterpret_cast<const unsigned*>(words);
#pragma unroll
                    for (int word = 0; word < 8; ++word) {
                        const int offset = thread * 32 + word * 4;
                        const int chunk = offset >> 9, fragLane = (offset >> 4) & 31, q = (offset >> 2) & 3;
                        const int n = chunk * 16 + 8 * (q >> 1) + (fragLane >> 2), kBase = 4 * (fragLane & 3) + 16 * (q & 1);
                        const int c = n & 31, rest = c & 15;
                        const int slot = ((rest & 7) >> 1) * 24 + (n >> 5) * 8 + (c >> 4) * 4 + 2 * (rest >> 3) + (rest & 1);
#pragma unroll
                        for (int byte = 0; byte < 4; ++byte) { Bs[kBase + byte][slot] = E4M3(w[word] >> (8 * byte)); }
                    }
                }
                __syncthreads();
                const float4* aRow = reinterpret_cast<const float4*>(&As[0][(warp * 8 + g) * 4]);
                const float4* bRow = reinterpret_cast<const float4*>(&Bs[0][t * 24]);
#pragma unroll 4
                for (int k = 0; k < 32; ++k) {
                    const float4 a4 = aRow[k * 32];
                    const float a[4] = {a4.x, a4.y, a4.z, a4.w};
                    float b[24];
#pragma unroll
                    for (int j = 0; j < 6; ++j) {
                        const float4 b4 = bRow[k * 24 + j];
                        b[4 * j] = b4.x; b[4 * j + 1] = b4.y; b[4 * j + 2] = b4.z; b[4 * j + 3] = b4.w;
                    }
#pragma unroll
                    for (int i = 0; i < 4; ++i) {
#pragma unroll
                        for (int j = 0; j < 24; ++j) { acc[i][j] = fmaf(a[i], b[j], acc[i][j]); }
                    }
                }
            }
            const float scale = Half(reinterpret_cast<const float*>(p.weights)[head]);
#pragma unroll
            for (int i = 0; i < 4; ++i) {
#pragma unroll
                for (int j = 0; j < 24; ++j) { acc[i][j] = Half(acc[i][j]); }
#pragma unroll
                for (int s = 0; s < 2; ++s) {   // Q, K
                    float sum = 0.0f;
#pragma unroll
                    for (int j = 0; j < 8; ++j) { sum += Half(acc[i][s * 8 + j] * acc[i][s * 8 + j]); }
                    sum += __shfl_xor_sync(0xFFFFFFFFu, sum, 2);
                    sum += __shfl_xor_sync(0xFFFFFFFFu, sum, 1);
                    const float r = Half(rsqrtf(fmaxf(Half(sum), eps)));
#pragma unroll
                    for (int j = 0; j < 8; ++j) {
                        const float x = Half(acc[i][s * 8 + j] * r);
                        acc[i][s * 8 + j] = s == 0 ? Half(Half(x * root32) * scale) : x;
                    }
                }
            }
            // Q in the accumulator output layout; K as an mma B operand with tokens as n (words
            // 1, 2 of each lane swapped); V transposed, below.
            unsigned char* const outputs[2] = {p.q, p.k};
#pragma unroll
            for (int bi = 0; bi < 2; ++bi) {
                const int block = tokenTile * 8 + warp * 2 + bi;
                if (block >= blocks16) { continue; }
#pragma unroll
                for (int s = 0; s < 2; ++s) {
                    unsigned words[4];
#pragma unroll
                    for (int r = 0; r < 4; ++r) {   // word r: row g (+8 if r&1), stored positions 4t.. (+16 if r&2)
                        const int row = bi * 2 + (r & 1), col = s * 8 + (r >> 1) * 4;
                        words[r] = EncodeE4M3(acc[row][col]) | (EncodeE4M3(acc[row][col + 1]) << 8) |
                                   (EncodeE4M3(acc[row][col + 2]) << 16) | (EncodeE4M3(acc[row][col + 3]) << 24);
                    }
                    *reinterpret_cast<uint4*>(outputs[s] + static_cast<size_t>(block) * 16 * 1024 + head * 512 + lane * 16) =
                        s == 0 ? make_uint4(words[0], words[1], words[2], words[3]) : make_uint4(words[0], words[2], words[1], words[3]);
                }
            }
            // V^T: per 32-token slice, 32 heads x 1024 bytes; per head two 512-byte chunks of 16
            // dims in the weights' B layout, tokens as k (in stored-position order): lane 4g+t
            // byte q holds dim 8(q>>3) + g, slice position 4t + 16((q>>2)&1) + (q&3).
            // Staged as f32 in As[dim][token] and encoded while gathering.
            __syncthreads();
#pragma unroll
            for (int i = 0; i < 4; ++i) {
                const int token = (warp * 2 + (i >> 1)) * 16 + g + 8 * (i & 1);
#pragma unroll
                for (int j = 0; j < 8; ++j) {   // stored position 16(j>>2) + 4t + (j&3)
                    const int dim = 16 * (j >> 2) + 2 * t + (j & 1) + 8 * ((j >> 1) & 1);
                    As[dim][token] = acc[i][16 + j];
                }
            }
            __syncthreads();
            // Chunk word r of lane 4g+t: dim 16*half + 8(r>>1) + g, slice positions 4t + 16(r&1)
            // + 0..3, i.e. tokens 16(r&1) + 2t + {0, 1, 8, 9}. Chunks thread and thread + 128:
            // with chunks 2*thread + c, ptxas -O3 (CUDA 12.9, sm_61 and sm_89) built nvcc's sm_61
            // PTX so that a quarter of V came out wrong; -O0 and sm_89 PTX were right.
#pragma unroll
            for (int c = 0; c < 2; ++c) {
                const int chunk = thread + 128 * c, s = chunk >> 6, half = (chunk >> 5) & 1, l = chunk & 31;
                const int slice = tokenTile * 4 + s;
                if (slice * 2 >= blocks16) { continue; }
                unsigned words[4];
#pragma unroll
                for (int r = 0; r < 4; ++r) {
                    const float* row = &As[16 * half + 8 * (r >> 1) + (l >> 2)][s * 32 + 16 * (r & 1) + 2 * (l & 3)];
                    words[r] = EncodeE4M3(row[0]) | (EncodeE4M3(row[1]) << 8) | (EncodeE4M3(row[8]) << 16) | (EncodeE4M3(row[9]) << 24);
                }
                *reinterpret_cast<uint4*>(p.v + static_cast<size_t>(slice) * 32768 + head * 1024 + half * 512 + l * 16) =
                    make_uint4(words[0], words[1], words[2], words[3]);
            }
        }
        __threadfence();
        __syncthreads();
        if (thread == 0) {   // pairFlags start at -1 (cc_cb_clear)
            const int index = tokenTile * 16 + pair;
            const bool last = gridDim.z == 1 || atomicAdd(p.pairFlags + index, 1) == 0;
            if (Signal && last && p.signalFlags) {
                __threadfence();
                *reinterpret_cast<volatile int*>(p.signalFlags + index) = 0;
            }
        }
    }
}

// Builds can leave kernels to NVIDIA with -DTRP_SKIP_EXPAND, -DTRP_SKIP_CONTRACT, -DTRP_SKIP_QKV.
#ifndef TRP_SKIP_EXPAND
extern "C" __global__ void __launch_bounds__(128) cc_vit_1d_ffn_expand_fp8(Conv1x1Params p) { FfnExpand<false, false>(p); }
extern "C" __global__ void __launch_bounds__(128) cc_vit_1d_ffn_expand_publish_fp8(Conv1x1Params p) { FfnExpand<false, true>(p); }
extern "C" __global__ void __launch_bounds__(128) cc_vit_1d_ffn_expand_chained_fp8(Conv1x1Params p) { FfnExpand<true, true>(p); }
#endif
#ifndef TRP_SKIP_CONTRACT
extern "C" __global__ void __launch_bounds__(128) cc_vit_1d_ffn_contract_fp8(ContractParams p) { FfnContract<false, false>(p); }
extern "C" __global__ void __launch_bounds__(128) cc_vit_1d_ffn_contract_chained_fp8(ContractParams p) { FfnContract<true, true>(p); }
#endif
#ifndef TRP_SKIP_QKV
extern "C" __global__ void __launch_bounds__(128) cc_vit_1d_qkv_fp8(QkvParams p) { Qkv<false, false>(p); }
extern "C" __global__ void __launch_bounds__(128) cc_vit_1d_qkv_chained_fp8(QkvParams p) { Qkv<true, true>(p); }
#endif
