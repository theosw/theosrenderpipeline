// Research: copies one buffer to another in 16-byte units, for TRP_NR_KERNEL_CAPTURE.
struct CopyParams { const uint4* source; uint4* target; unsigned long long count; };
extern "C" __global__ void __launch_bounds__(256) trp_copy(CopyParams p)
{
    for (unsigned long long i = blockIdx.x * 256ull + threadIdx.x; i < p.count; i += gridDim.x * 256ull) {
        p.target[i] = __ldcg(p.source + i);
    }
}
