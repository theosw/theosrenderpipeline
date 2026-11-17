// Deep decoder: canonical standard KHR operations, never native component access.
#define NR_KEEP_ACC coopmat<float,gl_ScopeSubgroup,16,16,gl_MatrixUseAccumulator>
NR_KEEP_ACC nr_keep_load(float c[8]) {
    uint base=gl_SubgroupID*256u,lane=gl_SubgroupInvocationID;
    for(uint i=0;i<8;++i)nr_port_c[base+(2u*i+(lane>>4u))*16u+(lane&15u)]=c[i];
    subgroupMemoryBarrierShared();subgroupBarrier();
    NR_KEEP_ACC cc;
    coopMatLoad(cc,nr_port_c,base,16u,gl_CooperativeMatrixLayoutRowMajor);
    subgroupBarrier();return cc;
}
float[8] nr_keep_read(NR_KEEP_ACC cc) {
    uint base=gl_SubgroupID*256u,lane=gl_SubgroupInvocationID;
    coopMatStore(cc,nr_port_c,base,16u,gl_CooperativeMatrixLayoutRowMajor);
    subgroupMemoryBarrierShared();subgroupBarrier();
    float r[8];for(uint i=0;i<8;++i)r[i]=nr_port_c[base+(2u*i+(lane>>4u))*16u+(lane&15u)];
    subgroupBarrier();return r;
}
NR_KEEP_ACC nr_keep_mma(float16_t a[16],float16_t b[16],NR_KEEP_ACC cc) {
    uint base=gl_SubgroupID*256u,lane=gl_SubgroupInvocationID;
    if(lane<16u)for(uint k=0;k<16u;++k){nr_port_a[base+k*16u+lane]=a[k];nr_port_b[base+k*16u+lane]=b[k];}
    subgroupMemoryBarrierShared();subgroupBarrier();
    coopmat<float16_t,gl_ScopeSubgroup,16,16,gl_MatrixUseA> aa;
    coopmat<float16_t,gl_ScopeSubgroup,16,16,gl_MatrixUseB> bb;
    coopMatLoad(aa,nr_port_a,base,16u,gl_CooperativeMatrixLayoutColumnMajor);
    coopMatLoad(bb,nr_port_b,base,16u,gl_CooperativeMatrixLayoutRowMajor);
    cc=coopMatMulAdd(aa,bb,cc);subgroupBarrier();return cc;
}
#define NR_KEEP_MMA(c,a,b) c=nr_keep_mma(a,b,c)
#define NR_KEEP_A coopmat<float16_t,gl_ScopeSubgroup,16,16,gl_MatrixUseA>
#define NR_KEEP_B coopmat<float16_t,gl_ScopeSubgroup,16,16,gl_MatrixUseB>
NR_KEEP_A nr_keep_operand_a(float16_t a[16]) {
    uint base=gl_SubgroupID*256u,lane=gl_SubgroupInvocationID;
    if(lane<16u)for(uint k=0;k<16u;++k)nr_port_a[base+k*16u+lane]=a[k];
    subgroupMemoryBarrierShared();subgroupBarrier();
    NR_KEEP_A aa;coopMatLoad(aa,nr_port_a,base,16u,gl_CooperativeMatrixLayoutColumnMajor);
    subgroupBarrier();return aa;
}
