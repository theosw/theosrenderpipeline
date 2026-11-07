// Grouped middle linear: native accumulators span all eight hidden K tiles.
// eqg already holds quantized software B operands; stage each only once per K.
#if !NR_HWAVES || NR_ACC_F16 || NR_F16_MMA || NR_MF != 4 || NR_DF != 2
#error "Window middle core requires the existing FP32 head-split shape"
#endif
{
#define NR_NATIVE_CUSTOM_IO 1
#define NR_MFRAG 2
#define NR_NFRAG 4
#define NR_NATIVE_K_LIMIT uint(NR_HG)
#define NR_NATIVE_LOAD_A(i) coopMatLoad(native_a[i],wgt_e4m3,NR_TILE(pc.mid_off,uint(g*NR_DF+i)*16u,kk,uint(NR_HG)),16u,gl_CooperativeMatrixLayoutRowMajor);
#define NR_NATIVE_LOAD_B(j) { \
    if(lane<16u)for(uint e=0u;e<16u;++e)nr_port_b[native_base+e*16u+lane]=eqg[j][kk/16u][e]; \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    coopMatLoad(native_b[j],nr_port_b,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupBarrier(); }
#define acc nr_mid_acc
#include "portable_gemm_core.glsl"
#undef acc
#undef NR_MFRAG
#undef NR_NFRAG
}
