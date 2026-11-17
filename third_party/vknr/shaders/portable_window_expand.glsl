// Reuse deep core10's opaque full-K accumulator loop. Weight tiles are [out][K];
// the x exchange tiles are [token][K] and become B via a standard ColumnMajor load.
#if !NR_HWAVES || NR_ACC_F16 || NR_F16_MMA || NR_EXPAND_GROUP != 4 || NR_MF != 4 || NR_SWI4
#error "Window expansion core requires the existing FP32 four-by-four head-split shape"
#endif
{
#define NR_NATIVE_CUSTOM_IO 1
#define NR_MFRAG 4
#define NR_NFRAG 4
#define NR_NATIVE_K_LIMIT uint(NR_C)
#define NR_NATIVE_K_LOOP NR_EXPAND_KLOOP
#define NR_NATIVE_LOAD_A(i) coopMatLoad(native_a[i],wgt_e4m3,NR_TILE(pc.e_off,uint(g*NR_HGF+h+i)*16u,kk,uint(NR_C)),16u,gl_CooperativeMatrixLayoutRowMajor);
#define NR_NATIVE_LOAD_B(j) coopMatLoad(native_b[j],lds_x,NR_LXB_ (uint(j*NR_CF)+kk/16u)*256u+NR_OPQ(1u+uint(h)),16u,gl_CooperativeMatrixLayoutColumnMajor);
#define NR_NATIVE_MMA_J(j) NR_MG(j) native_acc[ii][j]=coopMatMulAdd(native_a[ii],native_b[j],native_acc[ii][j]);
#define acc a
#include "portable_gemm_core.glsl"
#undef acc
#undef NR_MFRAG
#undef NR_NFRAG
}
