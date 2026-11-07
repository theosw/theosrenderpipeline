"""Deep-owned decoder specialization. Original and native RDNA3 bodies stay intact.

Retains standard opaque KHR accumulators, uses canonical matrix loads and makes
independent fragment indices compile-time constants. Quantization epilogues and
ascending reductions remain unchanged. Applies only to explicitly selected
portable fswinfusedup pipelines; every replacement fails closed on source drift.
"""
from pathlib import Path
import os

def apply(portable, root):
    portable=Path(portable);root=Path(root)
    parts=frozenset(['up', 'expand', 'reuse', 'direct', 'contract', 'directct', 'project', 'directop', 'qkv', 'directqkv', 'stream', 'unroll'])
    changes=[]
    t=(portable/"include/fswin_body.glsl").read_text()
    def retain(needle,name,dimensions,iterator,index,offset=0,tag=''):
        nonlocal t
        start=t.index(needle,offset);brace=t.index('{',start);level=1;end=brace+1
        while level:
            if t[end]=='{':level+=1
            if t[end]=='}':level-=1
            end+=1
        body=t[start:end]
        import re
        pattern=r'NR_MMA\(('+re.escape(name)+r'(?:\[[^\]]+\])*)\s*,'
        native='nr_keep_'+tag+name
        body,n=re.subn(pattern,lambda m:'NR_KEEP_MMA('+native+m.group(1)[len(name):]+',',body)
        assert n,(needle,name)
        before=f'NR_KEEP_ACC {native}{dimensions};\n{iterator} {native}{index}=nr_keep_load({name}{index});\n'
        after=f'\n{iterator} {name}{index}=nr_keep_read({native}{index});\n'
        t=t[:start]+before+body+after+t[end:]
        changes.append(dict(needle=needle,name=name,mma_sites=n))

    if 'up' in parts:
        retain('for(int k=0;k<4;++k) {\n            NR_FRAG_A w;','acc','','','')
        retain('for(int k=0;k<2*NR_CF;++k) {\n            NR_FRAG_A w;','acc','','','')
    if 'expand' in parts:
        retain('NR_EXPAND_KLOOP for (int k = 0; k < NR_CF; ++k) {','a','[NR_EXPAND_GROUP][NR_MF]','for(int p=0;p<NR_EXPAND_GROUP;++p) for(int m=0;m<NR_MF;++m) NR_MG(m)','[p][m]')
    if 'contract' in parts:
        retain('for (int k = 0; k < NR_KCF; ++k) {\n#if NR_HWAVES','a','[NR_MF]','for(int m=0;m<NR_MF;++m) NR_MG(m)','[m]',tag='ct_')
    if 'project' in parts:
        retain('for (int k = 0; k < NR_CF; ++k) {\n#if NR_HWAVES\n            NR_FRAG_B cqk','a','[NR_MF]','for(int m=0;m<NR_MF;++m) NR_MGA(m)','[m]',tag='op_')
    if 'qkv' in parts:
        anchor=t.index('#define NR_QK_ROWS ((1 + NR_QK_TOGETHER) * NR_DF)')
        retain('for (int k = 0; k < NR_CF; ++k) {\n                NR_FRAG_B yk[NR_MF];','acc','[NR_MF][NR_QK_ROWS]','for(int m=0;m<NR_MF;++m) for(int d=0;d<NR_QK_ROWS;++d) NR_MGA(m)','[m][d]',anchor,tag='qk_')
        retain('for (int k = 0; k < NR_CF; ++k) {\n#if NR_V_SWAP','acc','[NR_MF][NR_DF]','for(int m=0;m<NR_MF;++m) for(int d=0;d<NR_DF;++d) NR_MGA(m)','[m][d]',anchor,tag='v_')
    if 'stream' in parts:
        retain('NR_EXPAND_HLOOP for (int h = 0; h < NR_HF; ++h) {','stream_contract','[NR_CF][NR_MF]','for(int n=0;n<NR_CF;++n) for(int m=0;m<NR_MF;++m)','[n][m]')
    if 'pv' in parts:
        anchor=t.index('// ctx^T = V^T . P^T;')
        retain('for (int j = 0; j < NR_JF; ++j) NR_MGJ(j) {','a','[NR_MF]','for(int m=0;m<NR_MF;++m) NR_MGA(m)','[m]',anchor,tag='pv_')
    if 'reuse' in parts:
        assert 'expand' in parts
        start=t.index('NR_KEEP_ACC nr_keep_a[NR_EXPAND_GROUP][NR_MF]')
        end=t.index('a[p][m]=nr_keep_read(nr_keep_a[p][m]);',start)
        region=t[start:end]
        needle='uint(m * NR_CF + k) * 256u + NR_OPQ(1u + uint(h)), 16u);'
        assert region.count(needle)==1
        region=region.replace(needle,needle+'''
                    NR_KEEP_B nr_expand_b[NR_MF];
                    for(int m=0;m<NR_MF;++m)
                        coopMatLoad(nr_expand_b[m],lds_x,NR_LXB_ uint(m*NR_CF+k)*256u+NR_OPQ(1u+uint(h)),16u,gl_CooperativeMatrixLayoutColumnMajor);
    ''')
        needle='for(int m=0;m<NR_MF;++m) NR_MG(m) NR_KEEP_MMA(nr_keep_a[p][m],wf0,xbk[m]);\n                    for(int m=0;m<NR_MF;++m) NR_MG(m) NR_KEEP_MMA(nr_keep_a[p+1][m],wf1,xbk[m]);'
        assert region.count(needle)==1
        region=region.replace(needle,'''NR_KEEP_A nr_expand_a0=nr_keep_operand_a(wf0),nr_expand_a1=nr_keep_operand_a(wf1);
                        for(int m=0;m<NR_MF;++m) NR_MG(m) nr_keep_a[p][m]=coopMatMulAdd(nr_expand_a0,nr_expand_b[m],nr_keep_a[p][m]);
                        for(int m=0;m<NR_MF;++m) NR_MG(m) nr_keep_a[p+1][m]=coopMatMulAdd(nr_expand_a1,nr_expand_b[m],nr_keep_a[p+1][m]);''')
        needle='for (int m = 0; m < NR_MF; ++m) NR_KEEP_MMA(nr_keep_a[p][m], wf, xbk[m]);'
        assert region.count(needle)==1
        region=region.replace(needle,'''NR_KEEP_A nr_expand_a=nr_keep_operand_a(wf);
                        for(int m=0;m<NR_MF;++m) nr_keep_a[p][m]=coopMatMulAdd(nr_expand_a,nr_expand_b[m],nr_keep_a[p][m]);''')
        t=t[:start]+region+t[end:]
        changes.append(dict(native_operand_reuse='wide expansion',direct_B='ColumnMajor canonical shared/SSBO x',retained_A='one per paired weight row'))
    if 'direct' in parts:
        assert 'reuse' in parts
        needle='NR_KEEP_A nr_expand_a=nr_keep_operand_a(wf);'
        assert t.count(needle)==1
        t=t.replace(needle,'''NR_KEEP_A nr_expand_a;
                        coopMatLoad(nr_expand_a,NR_WARENA,NR_TILE(pc.e_off,uint(g*NR_HGF+h+p)*16u,uint(k)*16u,uint(NR_C)),16u,gl_CooperativeMatrixLayoutRowMajor);''')
        changes.append(dict(direct_weight_A='wide expansion, canonical tile-blocked weights'))
    if 'directct' in parts:
        assert 'contract' in parts
        needle='for (int m = 0; m < NR_MF; ++m) NR_MG(m) NR_KEEP_MMA(nr_keep_ct_a[m], NR_WPF_WF_CT, NR_CT_SRC(m, k));'
        assert t.count(needle)==1
        t=t.replace(needle,'''#if NR_HWAVES
                NR_KEEP_A nr_ct_weight;
                coopMatLoad(nr_ct_weight,NR_WARENA,NR_TILE(pc.ct_off,uint(n)*16u,uint(k)*16u,uint(NR_KCF*16)),16u,gl_CooperativeMatrixLayoutRowMajor);
                for(int m=0;m<NR_MF;++m) NR_MG(m) {
                    NR_KEEP_B nr_ct_input;
                    coopMatLoad(nr_ct_input,lds_y,NR_LXB_ uint(m*NR_CF+k)*256u+NR_OPQ(1u+uint(nn)),16u,gl_CooperativeMatrixLayoutColumnMajor);
                    nr_keep_ct_a[m]=coopMatMulAdd(nr_ct_weight,nr_ct_input,nr_keep_ct_a[m]);
                }
    #else
                '''+needle+'''
    #endif''')
        changes.append(dict(direct_operands='wide contraction, canonical weights and shared/SSBO y'))
    if 'directop' in parts:
        assert 'project' in parts
        needle='for (int m = 0; m < NR_MF; ++m) NR_MGA(m) NR_KEEP_MMA(nr_keep_op_a[m], NR_WPF_WF_OP, NR_CQ(m, k));'
        assert t.count(needle)==1
        t=t.replace(needle,'''#if NR_HWAVES
                NR_KEEP_A nr_op_weight;
                coopMatLoad(nr_op_weight,NR_WARENA,NR_TILE(pc.op_off,uint(n)*16u,uint(k)*16u,uint(NR_C)),16u,gl_CooperativeMatrixLayoutRowMajor);
                for(int m=0;m<NR_MF;++m) NR_MGA(m) {
                    NR_KEEP_B nr_op_input;
                    coopMatLoad(nr_op_input,lds_y,NR_LXB_ uint(m*NR_CF+k)*256u+NR_OPQ(64u+uint(nn)),16u,gl_CooperativeMatrixLayoutColumnMajor);
                    nr_keep_op_a[m]=coopMatMulAdd(nr_op_weight,nr_op_input,nr_keep_op_a[m]);
                }
    #else
                '''+needle+'''
    #endif''')
        changes.append(dict(direct_operands='wide output projection, canonical weights and shared/SSBO y'))
    if 'directqkv' in parts:
        assert 'qkv' in parts
        needle='for (int m = 0; m < NR_MF; ++m) NR_KEEP_MMA(nr_keep_qk_acc[m][r], wf, yk[m]);'
        assert t.count(needle)==1
        t=t.replace(needle,'''NR_KEEP_A nr_qk_weight;
                        coopMatLoad(nr_qk_weight,NR_WARENA,NR_TILE(pc.qkv_off,uint(hh*3*NR_DF+r)*16u,uint(k)*16u,uint(NR_C)),16u,gl_CooperativeMatrixLayoutRowMajor);
                        for(int m=0;m<NR_MF;++m) {
                            NR_KEEP_B nr_qk_input;
                            coopMatLoad(nr_qk_input,lds_x,NR_LXB_ uint(m*NR_CF+k)*256u+NR_OPQ(128u),16u,gl_CooperativeMatrixLayoutColumnMajor);
                            nr_keep_qk_acc[m][r]=coopMatMulAdd(nr_qk_weight,nr_qk_input,nr_keep_qk_acc[m][r]);
                        }''')
        needle='for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_KEEP_MMA(nr_keep_v_acc[m][r],yka[m],wb0);\n                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_KEEP_MMA(nr_keep_v_acc[m][r+1],yka[m],wb1);'
        assert t.count(needle)==1
        t=t.replace(needle,'''NR_KEEP_B nr_v_weight0,nr_v_weight1;
                        coopMatLoad(nr_v_weight0,NR_WARENA,NR_TILE(pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+r)*16u,uint(k)*16u,uint(NR_C)),16u,gl_CooperativeMatrixLayoutColumnMajor);
                        coopMatLoad(nr_v_weight1,NR_WARENA,NR_TILE(pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+r+1)*16u,uint(k)*16u,uint(NR_C)),16u,gl_CooperativeMatrixLayoutColumnMajor);
                        for(int m=0;m<NR_MF;++m) NR_MGA(m) {
                            NR_KEEP_A nr_v_input;
                            coopMatLoad(nr_v_input,lds_x,NR_LXB_ uint(m*NR_CF+k)*256u+NR_OPQ(130u),16u,gl_CooperativeMatrixLayoutRowMajor);
                            nr_keep_v_acc[m][r]=coopMatMulAdd(nr_v_input,nr_v_weight0,nr_keep_v_acc[m][r]);
                            nr_keep_v_acc[m][r+1]=coopMatMulAdd(nr_v_input,nr_v_weight1,nr_keep_v_acc[m][r+1]);
                        }''')
        changes.append(dict(direct_operands='wide QK/V projections, canonical weights and shared/SSBO x'))
    if 'reusepv' in parts:
        assert 'pv' in parts
        needle='for (int m = 0; m < NR_MF; ++m) NR_MGA(m) NR_KEEP_MMA(nr_keep_pv_a[m], vf, pb[m][j]);'
        assert t.count(needle)==1
        t=t.replace(needle,'''NR_KEEP_A nr_pv_value=nr_keep_operand_a(vf);
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) {
                        NR_KEEP_B nr_pv_probability=nr_keep_operand_b(pb[m][j]);
                        nr_keep_pv_a[m]=coopMatMulAdd(nr_pv_value,nr_pv_probability,nr_keep_pv_a[m]);
                    }''')
        changes.append(dict(reused_operand='PV value matrix once per key tile, probabilities unchanged'))
    if 'unroll' in parts:
        import re
        pattern=r'(?<!\]\] )for\s*\(\s*int\s+(?:m|p|r|d)\s*=\s*0\s*;[^;]*(?:NR_MF|NR_DF|NR_QK_ROWS|NR_EXPAND_GROUP)[^;]*;[^)]*\)'
        t,n=re.subn(pattern,lambda m:'[[unroll]] '+m.group(0),t)
        changes.append(dict(explicit_unroll_independent_fragment_loops=n,unchanged_reduction_order=True))
    if 'rolledk' in parts:
        import re
        pattern=r'for\s*\(\s*int\s+k\s*=\s*0\s*;[^;]*NR_(?:CF|KCF)[^;]*;[^)]*\)'
        t,n=re.subn(pattern,lambda m:'[[dont_unroll]] '+m.group(0),t)
        changes.append(dict(rolled_reduction_loops=n,unchanged_reduction_order=True))
    (portable/'include/deep_decoder_body.glsl').write_text(t)
    (portable/'include/deep_fused_keep.glsl').write_bytes((root/'shaders/portable_decoder_keep.glsl').read_bytes())
    entry=portable/'fswin_t.comp';t=entry.read_text()
    needle='#include "coopmm.glsl"'
    assert t.count(needle)==1 and 'deep_fused_keep.glsl' not in t
    t=t.replace(needle,needle+'\n#if defined(NR_DEEP_DECODER)\n#if NR_ACC_F16 != 0\n#error "Deep retained core needs unchanged FP32 accumulation"\n#endif\n#include "deep_fused_keep.glsl"\n#endif')
    t=t.replace('#version 460','#version 460\n#if defined(NR_DEEP_DECODER)\n#extension GL_EXT_control_flow_attributes : require\n#endif')
    needle='#include "fswin_body.glsl"'
    assert t.count(needle)==6
    t=t.replace(needle,'#if defined(NR_DEEP_DECODER)\n#include "deep_decoder_body.glsl"\n#else\n'+needle+'\n#endif')
    entry.write_text(t)
