// C32 token-split QKV: share each canonical operand over all output fragments,
// retaining opaque accumulators for both K tiles. Norm and quantization stay outside.
#if NR_C != 32 || NR_HWAVES || NR_ACC_F16 || NR_F16_MMA || NR_DF != 2 || NR_WPF || NR_WSHARE
#error "C32 QKV core requires the existing FP32 token-split path"
#endif
{
#define NR_NATIVE_CUSTOM_IO 1
#define NR_MFRAG 4
#define NR_NFRAG 1
#define NR_NATIVE_K_LIMIT uint(NR_C)
#define NR_NATIVE_LOAD_A(i) coopMatLoad(native_a[i],wgt_e4m3,NR_TILE(pc.qkv_off,uint(hh*3*NR_DF+i)*16u,kk,uint(NR_C)),16u,gl_CooperativeMatrixLayoutRowMajor);
#define NR_NATIVE_LOAD_B(j) { \
    if(lane<16u)for(uint e=0u;e<16u;++e)nr_port_b[native_base+e*16u+lane]=NR_QKV_SRC(m,int(kk/16u))[e]; \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    coopMatLoad(native_b[j],nr_port_b,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupBarrier(); }
#define NR_NATIVE_MMA_J(j) NR_MGA(m) native_acc[ii][j]=coopMatMulAdd(native_a[ii],native_b[j],native_acc[ii][j]);
#define NR_NATIVE_READ_J(j) { \
    coopMatStore(native_acc[ii][j],nr_port_c,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    for(uint e=0u;e<8u;++e)qkv[ii][e]=nr_port_c[native_base+(2u*e+(lane>>4u))*16u+(lane&15u)]; \
    subgroupBarrier(); }
#include "portable_gemm_core.glsl"
#undef NR_MFRAG
#undef NR_NFRAG
}
{
#define NR_NATIVE_CUSTOM_IO 1
#define NR_NATIVE_K_LIMIT uint(NR_C)
#if NR_V_SWAP
#define NR_MFRAG 1
#define NR_NFRAG 2
#define NR_NATIVE_LOAD_A(i) { \
    if(lane<16u)for(uint e=0u;e<16u;++e)nr_port_a[native_base+lane*16u+e]=yqa[m][kk/16u][e]; \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    coopMatLoad(native_a[i],nr_port_a,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupBarrier(); }
#define NR_NATIVE_LOAD_B(j) coopMatLoad(native_b[j],wgt_e4m3,NR_TILE(pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+j)*16u,kk,uint(NR_C)),16u,gl_CooperativeMatrixLayoutColumnMajor);
#define NR_WINDOW_V_INDEX(j) j
#else
#define NR_MFRAG 2
#define NR_NFRAG 1
#define NR_NATIVE_LOAD_A(i) coopMatLoad(native_a[i],wgt_e4m3,NR_TILE(pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+i)*16u,kk,uint(NR_C)),16u,gl_CooperativeMatrixLayoutRowMajor);
#define NR_NATIVE_LOAD_B(j) { \
    if(lane<16u)for(uint e=0u;e<16u;++e)nr_port_b[native_base+e*16u+lane]=NR_QKV_SRC(m,int(kk/16u))[e]; \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    coopMatLoad(native_b[j],nr_port_b,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupBarrier(); }
#define NR_WINDOW_V_INDEX(j) ii
#endif
#define NR_NATIVE_MMA_J(j) NR_MGA(m) native_acc[ii][j]=coopMatMulAdd(native_a[ii],native_b[j],native_acc[ii][j]);
#define NR_NATIVE_READ_J(j) { \
    coopMatStore(native_acc[ii][j],nr_port_c,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    for(uint e=0u;e<8u;++e)qkv[2*NR_DF+NR_WINDOW_V_INDEX(j)][e]=nr_port_c[native_base+(2u*e+(lane>>4u))*16u+(lane&15u)]; \
    subgroupBarrier(); }
#include "portable_gemm_core.glsl"
#undef NR_MFRAG
#undef NR_NFRAG
#undef NR_WINDOW_V_INDEX
}
