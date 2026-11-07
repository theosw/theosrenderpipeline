// Full-K QK/V projection using the shared deep accumulator core. The normalization
// and quantization that follow this include are the untouched window epilogues.
#if !NR_HWAVES || NR_ACC_F16 || NR_F16_MMA || NR_MF != 4 || NR_DF != 2 || !NR_QK_TOGETHER || NR_SWQ4
#error "Window QKV core requires the existing FP32 head-split QK-together shape"
#endif
{
#define NR_NATIVE_CUSTOM_IO 1
#define NR_NATIVE_K_LIMIT uint(NR_C)
#if NR_WINDOW_QKV_V && NR_V_SWAP
#define NR_MFRAG 4
#define NR_NFRAG 2
#define NR_NATIVE_LOAD_A(i) coopMatLoad(native_a[i],lds_x,NR_LXB_ (uint(i*NR_CF)+kk/16u)*256u+NR_OPQ(130u),16u,gl_CooperativeMatrixLayoutRowMajor);
#define NR_NATIVE_LOAD_B(j) coopMatLoad(native_b[j],wgt_e4m3,NR_TILE(pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+j)*16u,kk,uint(NR_C)),16u,gl_CooperativeMatrixLayoutColumnMajor);
#define NR_NATIVE_MMA_J(j) NR_MGA(ii) native_acc[ii][j]=coopMatMulAdd(native_a[ii],native_b[j],native_acc[ii][j]);
#else
#if NR_WINDOW_QKV_V
#define NR_MFRAG 2
#else
#define NR_MFRAG 4
#endif
#define NR_NFRAG 4
#define NR_NATIVE_LOAD_A(i) coopMatLoad(native_a[i],wgt_e4m3,NR_TILE(pc.qkv_off,uint(hh*3*NR_DF+NR_WINDOW_QKV_V*2*NR_DF+i)*16u,kk,uint(NR_C)),16u,gl_CooperativeMatrixLayoutRowMajor);
#define NR_NATIVE_LOAD_B(j) coopMatLoad(native_b[j],lds_x,NR_LXB_ (uint(j*NR_CF)+kk/16u)*256u+NR_OPQ(128u),16u,gl_CooperativeMatrixLayoutColumnMajor);
#define NR_NATIVE_MMA_J(j) NR_MGA(j) native_acc[ii][j]=coopMatMulAdd(native_a[ii],native_b[j],native_acc[ii][j]);
#define NR_NATIVE_READ_J(j) { \
    coopMatStore(native_acc[ii][j],nr_port_c,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
    subgroupMemoryBarrierShared();subgroupBarrier(); \
    for(uint e=0u;e<8u;++e)acc[j][ii][e]=nr_port_c[native_base+(2u*e+(lane>>4u))*16u+(lane&15u)]; \
    subgroupBarrier(); }
#endif
#include "portable_gemm_core.glsl"
#undef NR_MFRAG
#undef NR_NFRAG
#undef NR_WINDOW_QKV_V
}
