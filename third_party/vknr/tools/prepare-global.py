"""Exact global attention: opaque retention and canonical operand reuse."""
import re

def apply(source,root):
    entry = source / 'vit_attn.comp'
    chunk = source / 'include/vit_attn_vt_chunk.glsl'
    t = entry.read_text()
    c = chunk.read_text()
    helper = (root / 'shaders/portable_decoder_keep.glsl').read_text()
    helper += '\n// Probability is already on the E4M3 grid, in the owned software C map.\n// Store the whole canonical [key][query] tile once; use it as opaque B.\nNR_KEEP_B nr_global_probability(float16_t ph[8]) {\n    uint base=gl_SubgroupID*256u,lane=gl_SubgroupInvocationID;\n    for(uint i=0;i<8;++i)nr_port_b[base+(2u*i+(lane>>4u))*16u+(lane&15u)]=ph[i];\n    subgroupMemoryBarrierShared();subgroupBarrier();\n    NR_KEEP_B value;coopMatLoad(value,nr_port_b,base,16u,gl_CooperativeMatrixLayoutRowMajor);\n    subgroupBarrier();return value;\n}\n#define NR_GLOBAL_LOAD_Q(f,a,o,s) coopMatLoad(f,a,o,s,gl_CooperativeMatrixLayoutColumnMajor)\n'
    (source / 'include/global_keep.glsl').write_text(helper)
    t = t.replace('#include "coopmm.glsl"', '#include "coopmm.glsl"\n#include "global_keep.glsl"')
    needle = '    NR_FRAG_ACC ctx[NR_QB][2];'
    assert t.count(needle) == 2
    t = t.replace(needle, needle + '\n    NR_KEEP_ACC global_ctx[NR_QB][2];\n    for(int b=0;b<NR_QB;++b) for(int n=0;n<2;++n) global_ctx[b][n]=NR_KEEP_ACC(0.0);', 1)
    start = t.index('    NR_FRAG_B qfr[NR_QB][2];')
    end = t.index('    f16vec2 part[NR_QB][4];', start)
    q = t[start:end].replace('NR_FRAG_B qfr', 'NR_KEEP_B qfr').replace('NR_LOAD_B(', 'NR_GLOBAL_LOAD_Q(').replace('nr_soft_operand(0.0)', 'NR_KEEP_B(float16_t(0.0))')
    t = t[:start] + q + t[end:]
    needle = '    // The original subtracts exp(0) for padded keys after summation.'
    assert t.count(needle) == 2
    t = t.replace(needle, '    for(int b=0;b<NR_QB;++b) for(int n=0;n<2;++n) ctx[b][n]=nr_keep_read(global_ctx[b][n]);\n\n' + needle, 1)
    start = c.index('            NR_FRAG_A kfr[2];')
    end = c.index('            // V^T as the A operand', start)
    c = c[:start] + '            NR_KEEP_A kfr[2];\n            for(uint d=0;d<2;++d) {\n                if(NR_VT_GUARD==0 || j0+kb<pc.tokens)\n                    coopMatLoad(kfr[d],act_e4m3,nr_at16(pc.x_off,j0+kb,kbase+d*16u,X3),16u,gl_CooperativeMatrixLayoutRowMajor);\n                else kfr[d]=NR_KEEP_A(float16_t(0.0));\n            }\n' + c[end:]
    needle = '                NR_FRAG_ACC lg = NR_ACC_ZERO;\n                for (uint d = 0u; d < 2u; ++d) NR_MMA(lg, kfr[d], qfr[b][d]);'
    assert c.count(needle) == 1
    c = c.replace(needle, '                NR_KEEP_ACC global_lg=NR_KEEP_ACC(0.0);\n                for(uint d=0;d<2;++d) global_lg=coopMatMulAdd(kfr[d],qfr[b][d],global_lg);\n                NR_FRAG_ACC lg=nr_keep_read(global_lg);')
    needle = 'NR_MMA(ctx[b][n], vf[n], pf);'
    assert c.count(needle) == 1
    if True:
        start = c.index('            NR_FRAG_A vf[2];')
        end = c.index('            for (uint b = 0u;', start)
        c = c[:start] + '            NR_KEEP_A vf[2];\n            for(uint n=0;n<2;++n)\n                coopMatLoad(vf[n],act_e4m3,nr_at16(pc.x_off,min(j0+kb,jlast),vbase+n*16u,X3),16u,gl_CooperativeMatrixLayoutColumnMajor);\n' + c[end:]
        start = c.index('                NR_FRAG_B pf;')
        end = c.index('                for (uint n = 0u;', start)
        c = c[:start] + '                NR_KEEP_B pf=nr_global_probability(ph);\n' + c[end:]
        c = c.replace(needle, 'global_ctx[b][n]=coopMatMulAdd(vf[n],pf,global_ctx[b][n]);')
    else:
        c = c.replace(needle, 'NR_KEEP_MMA(global_ctx[b][n],vf[n],pf);')
    if True:
        t = t.replace('#version 460', '#version 460\n#extension GL_EXT_control_flow_attributes : require')
        pattern = 'for\\s*\\(\\s*(?:uint|int)\\s+(?:b|d|n)\\s*=\\s*0u?\\s*;[^;]*;[^)]*\\)'
        t = re.sub(pattern, lambda m: '[[unroll]] ' + m.group(0), t)
        c = re.sub(pattern, lambda m: '[[unroll]] ' + m.group(0), c)
    entry.write_text(t)
    chunk.write_text(c)
