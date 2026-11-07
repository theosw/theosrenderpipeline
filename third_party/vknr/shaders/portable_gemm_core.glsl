// Portable KHR GEMM: retain opaque FP32 accumulators over the complete K loop.
// All element-addressed operations use owned canonical arrays, never matrix components.
// Existing host checks require the FP16/FP32 16x16x16 subgroup tuple and size 32.
// Shapes and epilogue remain the original graph's; no changed rounding boundary.
#if !defined(NR_NATIVE_CUSTOM_IO) && (NR_ACC_F16 || NR_SWAP_AB || NR_STAGED || !defined(NR_A_DIRECT) || !defined(NR_TILED_A) || !defined(NR_PACKED_WEIGHT_N2) || defined(NR_I4))
#error "Portable GEMM core requires direct tiled FP16 E4M3 operands, paired weights and FP32 accumulation"
#endif
#define NR_NATIVE_ACC coopmat<float,gl_ScopeSubgroup,16,16,gl_MatrixUseAccumulator>
#define NR_NATIVE_A coopmat<float16_t,gl_ScopeSubgroup,16,16,gl_MatrixUseA>
#define NR_NATIVE_B coopmat<float16_t,gl_ScopeSubgroup,16,16,gl_MatrixUseB>
// Optional canonical-I/O hooks let fullres use this same reduction loop.
// Hooks own loads and the final export, never opaque fragment components.
#ifndef NR_NATIVE_K
#define NR_NATIVE_K pc.K
#endif
#ifndef NR_NATIVE_K_LIMIT
#define NR_NATIVE_K_LIMIT NR_NATIVE_K
#endif
#ifndef NR_NATIVE_K_LOOP
#define NR_NATIVE_K_LOOP
#endif
#ifndef NR_NATIVE_K_BEGIN
#define NR_NATIVE_K_BEGIN 0u
#endif
// Streaming callers may supply an accumulator array initialized with their
// original residual and reduce one ascending K slice per invocation.
#ifndef NR_NATIVE_ACCUMULATORS
#define NR_NATIVE_ACCUMULATORS native_acc
    NR_NATIVE_ACC native_acc[NR_MFRAG][NR_NFRAG];
#ifndef NR_NATIVE_ZERO_J
#define NR_NATIVE_ZERO_J(j) NR_NATIVE_ACCUMULATORS[ii][j]=NR_NATIVE_ACC(0.0);
#endif
#define NR_NATIVE_ZERO_I(i) { const int ii=i; NR_FOR_J(NR_NFRAG,NR_NATIVE_ZERO_J) }
    NR_FOR_I(NR_MFRAG,NR_NATIVE_ZERO_I)
#endif
    const uint native_base=gl_SubgroupID*256u;
    NR_NATIVE_K_LOOP for(uint kk=NR_NATIVE_K_BEGIN;kk<NR_NATIVE_K_LIMIT;kk+=16u){
        NR_NATIVE_A native_a[NR_MFRAG];
        NR_NATIVE_B native_b[NR_NFRAG];
#ifndef NR_NATIVE_LOAD_A
#define NR_NATIVE_LOAD_A(i) coopMatLoad(native_a[i],act_e4m3,abase_g[i]+NR_AKOFF(kk),16u,gl_CooperativeMatrixLayoutRowMajor);
#endif
        NR_FOR_I(NR_MFRAG,NR_NATIVE_LOAD_A)
#ifndef NR_NATIVE_LOAD_B
#define NR_NATIVE_LOAD_B(j) { \
        const uint ch=ng0+nloc+uint(j)*16u; \
        const uint packed_base=pc.w_off+((ch/32u)*(pc.K/16u)+kk/16u)*512u+((ch/16u)&1u)*8u; \
        for(uint e=gl_SubgroupInvocationID;e<256u;e+=gl_SubgroupSize){ \
            const uint k=e/16u,n=e%16u; \
            nr_port_b[native_base+e]=wgt_e4m3[packed_base+n*16u+(k&7u)+(k>>3u)*256u]; \
        } \
        subgroupMemoryBarrierShared();subgroupBarrier(); \
        coopMatLoad(native_b[j],nr_port_b,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
        subgroupBarrier(); }
#endif
        NR_FOR_J(NR_NFRAG,NR_NATIVE_LOAD_B)
#ifndef NR_NATIVE_MMA_J
#define NR_NATIVE_MMA_J(j) NR_NATIVE_ACCUMULATORS[ii][j]=coopMatMulAdd(native_a[ii],native_b[j],NR_NATIVE_ACCUMULATORS[ii][j]);
#endif
#define NR_NATIVE_MMA_I(i) { const int ii=i; NR_FOR_J(NR_NFRAG,NR_NATIVE_MMA_J) }
        NR_FOR_I(NR_MFRAG,NR_NATIVE_MMA_I)
    }
#ifndef NR_NATIVE_READ_J
#define NR_NATIVE_READ_J(j) { \
        coopMatStore(NR_NATIVE_ACCUMULATORS[ii][j],nr_port_c,native_base,16u,gl_CooperativeMatrixLayoutRowMajor); \
        subgroupMemoryBarrierShared();subgroupBarrier(); \
        for(uint e=0u;e<8u;++e)acc[ii][j][e]=nr_port_c[native_base+(2u*e+(lane>>4u))*16u+(lane&15u)]; \
        subgroupBarrier(); }
#endif
#define NR_NATIVE_READ_I(i) { const int ii=i; NR_FOR_J(NR_NFRAG,NR_NATIVE_READ_J) }
    NR_FOR_I(NR_MFRAG,NR_NATIVE_READ_I)

#undef NR_NATIVE_K
#undef NR_NATIVE_LOAD_A
#undef NR_NATIVE_LOAD_B
#undef NR_NATIVE_READ_J
#undef NR_NATIVE_ZERO_J
#undef NR_NATIVE_ZERO_I
#undef NR_NATIVE_MMA_J
#undef NR_NATIVE_MMA_I
#undef NR_NATIVE_READ_I
#undef NR_NATIVE_ACC
#undef NR_NATIVE_A
#undef NR_NATIVE_B

#undef NR_NATIVE_ACCUMULATORS
#undef NR_NATIVE_K_BEGIN

#undef NR_NATIVE_CUSTOM_IO
#undef NR_NATIVE_K_LIMIT
#undef NR_NATIVE_K_LOOP
