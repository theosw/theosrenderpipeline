// Original compatibility layer. Arrays here have an explicitly defined software
// distribution. Native cooperative-matrix fragments are never indexed.
// NR_OP_KSWAP must be zero: duplicated operand lanes represent identical data.
#undef NR_FRAG_A
#undef NR_FRAG_B
#undef NR_FRAG_ACC
#undef NR_FRAG_A16
#undef NR_FRAG_B16
#undef NR_FRAG_ACC16
#undef NR_FRAG_E4M3
#define NR_FRAG_A float16_t[16]
#define NR_FRAG_B float16_t[16]
#define NR_FRAG_A16 float16_t[16]
#define NR_FRAG_B16 float16_t[16]
#define NR_FRAG_ACC float[8]
#define NR_FRAG_ACC16 float16_t[8]
#define NR_FRAG_E4M3 float16_t[8]

float[8] nr_soft_acc(float x) { float r[8]; for(int i=0;i<8;++i)r[i]=x;return r; }
float[8] nr_soft_acc(float x[8]) { return x; }
float[8] nr_soft_acc(float16_t x[8]) { float r[8];for(int i=0;i<8;++i)r[i]=float(x[i]);return r; }
float16_t[8] nr_soft_acc16(float x[8]) { float16_t r[8];for(int i=0;i<8;++i)r[i]=float16_t(nr_half_round(x[i]));return r; }
float16_t[8] nr_soft_acc16(float x) { return nr_soft_acc16(nr_soft_acc(x)); }
float16_t[8] nr_soft_acc16(float16_t x[8]) { return x; }
float16_t[16] nr_soft_operand(float16_t x[16]) {return x;}
float16_t[16] nr_soft_operand(float x) {float16_t r[16];for(int i=0;i<16;++i)r[i]=float16_t(nr_half_round(x));return r;}
void nr_soft_set(inout float x[8],uint i,float v){x[i]=v;}
void nr_soft_set(inout float16_t x[8],uint i,float v){x[i]=float16_t(nr_half_round(v));}

#undef NR_LOAD_A
#undef NR_LOAD_B
#undef NR_LOAD_B_ROW
#undef NR_LOAD_A_COL
#undef NR_LOAD_ACC
#undef NR_LOAD_ACC_COL
#undef NR_STORE_ACC
#undef NR_STORE_ACC_COL
#define NR_LOAD_A(f,a,o,s) {for(uint ni=0;ni<16;++ni)(f)[ni]=(a)[(o)+(gl_SubgroupInvocationID&15u)*(s)+ni];}
#define NR_LOAD_B(f,a,o,s) NR_LOAD_A(f,a,o,s)
#define NR_LOAD_A_COL(f,a,o,s) {for(uint ni=0;ni<16;++ni)(f)[ni]=(a)[(o)+ni*(s)+(gl_SubgroupInvocationID&15u)];}
#define NR_LOAD_B_ROW(f,a,o,s) NR_LOAD_A_COL(f,a,o,s)
#define NR_LOAD_ACC(f,a,o,s) {for(uint ni=0;ni<8;++ni)nr_soft_set(f,ni,float((a)[(o)+(2u*ni+(gl_SubgroupInvocationID>>4u))*(s)+(gl_SubgroupInvocationID&15u)]));}
#define NR_LOAD_ACC_COL(f,a,o,s) {for(uint ni=0;ni<8;++ni)nr_soft_set(f,ni,float((a)[(o)+(gl_SubgroupInvocationID&15u)*(s)+2u*ni+(gl_SubgroupInvocationID>>4u)]));}
#define NR_STORE_ACC(f,a,o,s) {for(uint ni=0;ni<8;++ni)(a)[(o)+(2u*ni+(gl_SubgroupInvocationID>>4u))*(s)+(gl_SubgroupInvocationID&15u)]=(f)[ni];}
#define NR_STORE_ACC_COL(f,a,o,s) {for(uint ni=0;ni<8;++ni)(a)[(o)+(gl_SubgroupInvocationID&15u)*(s)+2u*ni+(gl_SubgroupInvocationID>>4u)]=(f)[ni];}

// Each subgroup owns a separate canonical tile. Image kernels use exactly
// NR_FWAVES subgroups; reserving the general eight-subgroup maximum needlessly
// lowers occupancy. This is a launch-size bound, not a vendor tile assumption.
#if defined(NR_FUSED_IMAGE_INPUT) || defined(NR_FUSED_IMAGE_OUTPUT)
#define NR_PORT_STAGING_TILES NR_FWAVES
#else
#ifndef NR_PORT_SUBGROUPS
#define NR_PORT_SUBGROUPS 8
#endif
#define NR_PORT_STAGING_TILES NR_PORT_SUBGROUPS
#endif
shared float16_t nr_port_a[256 * NR_PORT_STAGING_TILES];
shared float16_t nr_port_b[256 * NR_PORT_STAGING_TILES];
shared float nr_port_c[256 * NR_PORT_STAGING_TILES];
float[8] nr_port_mma(float16_t a[16],float16_t b[16],float c[8]) {
    uint base=gl_SubgroupID*256u,lane=gl_SubgroupInvocationID;
    if(lane<16u)for(uint k=0;k<16u;++k){
#if defined(NR_FUSED_IMAGE_INPUT) || defined(NR_FUSED_IMAGE_OUTPUT) || NR_PORT_WINDOW_ACOL
        nr_port_a[base+k*16u+lane]=a[k];
#else
        nr_port_a[base+lane*16u+k]=a[k];
#endif
        nr_port_b[base+k*16u+lane]=b[k];}
    for(uint i=0;i<8;++i)nr_port_c[base+(2u*i+(lane>>4u))*16u+(lane&15u)]=c[i];
    subgroupMemoryBarrierShared();subgroupBarrier();
    coopmat<float16_t,gl_ScopeSubgroup,16,16,gl_MatrixUseA> aa;
    coopmat<float16_t,gl_ScopeSubgroup,16,16,gl_MatrixUseB> bb;
    coopmat<float,gl_ScopeSubgroup,16,16,gl_MatrixUseAccumulator> cc;
    #if defined(NR_FUSED_IMAGE_INPUT) || defined(NR_FUSED_IMAGE_OUTPUT) || NR_PORT_WINDOW_ACOL
    coopMatLoad(aa,nr_port_a,base,16,gl_CooperativeMatrixLayoutColumnMajor);
#else
    coopMatLoad(aa,nr_port_a,base,16,gl_CooperativeMatrixLayoutRowMajor);
#endif
    coopMatLoad(bb,nr_port_b,base,16,gl_CooperativeMatrixLayoutRowMajor);
    coopMatLoad(cc,nr_port_c,base,16,gl_CooperativeMatrixLayoutRowMajor);
    cc=coopMatMulAdd(aa,bb,cc);
    coopMatStore(cc,nr_port_c,base,16,gl_CooperativeMatrixLayoutRowMajor);
    subgroupMemoryBarrierShared();subgroupBarrier();
    float r[8];for(uint i=0;i<8;++i)r[i]=nr_port_c[base+(2u*i+(lane>>4u))*16u+(lane&15u)];
    subgroupBarrier();return r;
}

#if defined(NR_FUSED_IMAGE_INPUT) || defined(NR_FUSED_IMAGE_OUTPUT)
#define NR_PORT_IMAGE_CORE 1
#define NR_PORT_IMAGE_QKV 1
#define NR_PORT_IMAGE_CONTRACT 1
#endif
