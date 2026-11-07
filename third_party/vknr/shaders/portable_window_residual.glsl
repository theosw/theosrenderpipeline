// Seed the opaque accumulator once from the existing residual, retain it across
// K, then expose it once for the unchanged contraction/projection epilogue.
#if !NR_HWAVES || NR_ACC_F16 || NR_F16_MMA || NR_MF != 4 || NR_WPF
#error "Window residual core requires the existing FP32 head-split path"
#endif
{
#define NR_NATIVE_CUSTOM_IO 1
#define NR_MFRAG 1
#define NR_NFRAG 4
#define NR_NATIVE_K_LIMIT uint(NR_WINDOW_RESIDUAL_K*16)
#define NR_NATIVE_ZERO_J(j) { \
    const uint base=gl_SubgroupID*256u; \
    for(uint e=0u;e<8u;++e)nr_port_c[base+(2u*e+(lane>>4u))*16u+(lane&15u)]=a[j][e]; \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    coopMatLoad(native_acc[ii][j],nr_port_c,base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupBarrier(); }
#define NR_NATIVE_LOAD_A(i) coopMatLoad(native_a[i],wgt_e4m3,NR_TILE(NR_WINDOW_RESIDUAL_OFF,uint(n)*16u,kk,uint(NR_WINDOW_RESIDUAL_K*16)),16u,gl_CooperativeMatrixLayoutRowMajor);
#define NR_NATIVE_LOAD_B(j) coopMatLoad(native_b[j],lds_y,NR_LXB_ (uint(j*NR_CF)+kk/16u)*256u+NR_OPQ(NR_WINDOW_RESIDUAL_OPQ+uint(nn)),16u,gl_CooperativeMatrixLayoutColumnMajor);
#define NR_NATIVE_MMA_J(j) NR_WINDOW_RESIDUAL_MASK(j) native_acc[ii][j]=coopMatMulAdd(native_a[ii],native_b[j],native_acc[ii][j]);
#define NR_NATIVE_READ_J(j) { \
    coopMatStore(native_acc[ii][j],nr_port_c,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    for(uint e=0u;e<8u;++e)a[j][e]=nr_port_c[native_base+(2u*e+(lane>>4u))*16u+(lane&15u)]; \
    subgroupBarrier(); }
#include "portable_gemm_core.glsl"
#undef NR_MFRAG
#undef NR_NFRAG
#undef NR_WINDOW_RESIDUAL_K
#undef NR_WINDOW_RESIDUAL_OFF
#undef NR_WINDOW_RESIDUAL_OPQ
#undef NR_WINDOW_RESIDUAL_MASK
}
