"""Portable-only window transformations; original vendor bodies remain intact."""
def apply(portable,root):
    p=portable/'include/fswin_body.glsl'
    t=p.read_text()
    start='            for (int p = 0; p < NR_EXPAND_GROUP; ++p)\n                for (int m = 0; m < NR_MF; ++m) a[p][m] = NR_ACCZERO;\n'
    assert t.count(start)==1
    at=t.index(start)
    end='\n#endif\n            for (int p = 0; p < NR_EXPAND_GROUP; ++p)\n            for (int m = 0; m < NR_MF; ++m) NR_MG(m)'
    stop=t.index(end,at)
    old=t[at:stop]
    t=t[:at]+'#if NR_WINDOW_NATIVE_EXPAND\n#include "portable_window_expand.glsl"\n#else\n'+old+'\n#endif // NR_WINDOW_NATIVE_EXPAND'+t[stop:]
    start='            for (int m = 0; m < NR_MF; ++m)\n                for (int d = 0; d < NR_QK_ROWS; ++d) acc[m][d] = NR_ACCZERO;\n'
    assert t.count(start)==1
    at=t.index(start)
    end='\n#endif\n            for (int m = 0; m < NR_MF; ++m) NR_MGA(m) {\n#if NR_NORM_F32'
    stop=t.index(end,at)
    t=t[:at]+'#if NR_WINDOW_NATIVE_QKV\n#define NR_WINDOW_QKV_V 0\n#include "portable_window_qkv.glsl"\n#else\n'+t[at:stop]+'\n#endif // NR_WINDOW_NATIVE_QKV'+t[stop:]
    at=t.index('            // ---- pass V: rows')
    start='            for (int m = 0; m < NR_MF; ++m)\n                for (int d = 0; d < NR_DF; ++d) acc[m][d] = NR_ACCZERO;\n'
    at=t.index(start,at)
    end='\n#endif\n#if NR_V_SWAP\n            for (int m = 0; m < NR_MF; ++m) NR_MGK(m)'
    stop=t.index(end,at)
    t=t[:at]+'#if NR_WINDOW_NATIVE_QKV\n#define NR_WINDOW_QKV_V 1\n#include "portable_window_qkv.glsl"\n#else\n'+t[at:stop]+'\n#endif // NR_WINDOW_NATIVE_QKV'+t[stop:]
    at=t.index('    // ---- stage 2:')
    at=t.index('        for (int k = 0; k < NR_KCF; ++k) {',at)
    stop=t.index('\n#endif\n        // The residual is element-wise:',at)
    call='''#if (NR_WINDOW_NATIVE_RESIDUAL & 1) && NR_HWAVES
#define NR_WINDOW_RESIDUAL_K NR_KCF
#define NR_WINDOW_RESIDUAL_OFF pc.ct_off
#define NR_WINDOW_RESIDUAL_OPQ 1u
#define NR_WINDOW_RESIDUAL_MASK(j) NR_MG(j)
#include "portable_window_residual.glsl"
#else
'''
    t=t[:at]+call+t[at:stop]+'\n#endif // NR_WINDOW_NATIVE_RESIDUAL'+t[stop:]
    at=t.index('    // ---- stage 6:')
    at=t.index('        for (int k = 0; k < NR_CF; ++k) {',at)
    stop=t.index('\n#endif\n        // Again element-wise:',at)
    call='''#if (NR_WINDOW_NATIVE_RESIDUAL & 2) && NR_HWAVES
#define NR_WINDOW_RESIDUAL_K NR_CF
#define NR_WINDOW_RESIDUAL_OFF pc.op_off
#define NR_WINDOW_RESIDUAL_OPQ 64u
#define NR_WINDOW_RESIDUAL_MASK(j) NR_MGA(j)
#include "portable_window_residual.glsl"
#else
'''
    t=t[:at]+call+t[at:stop]+'\n#endif // NR_WINDOW_NATIVE_RESIDUAL'+t[stop:]
    at=t.index('#if (NR_PACKED_DENSE & 1) && NR_HWAVES')
    stop=t.index('\n#if NR_SWI4\n        // the e4m3 middle output',at)
    call='''#if NR_WINDOW_NATIVE_MIDDLE
        NR_ACCF nr_mid_acc[NR_DF][NR_MF];
#include "portable_window_middle.glsl"
#else
'''
    t=t[:at]+call+t[at:stop]+'\n#endif // NR_WINDOW_NATIVE_MIDDLE'+t[stop:]
    start='#if (NR_PACKED_DENSE & 1) && NR_HWAVES\n            NR_ACCF a[NR_MF];'
    assert t.count(start)==1
    t=t.replace(start,'#if ((NR_PACKED_DENSE & 1) && NR_HWAVES) || NR_WINDOW_NATIVE_MIDDLE\n            NR_ACCF a[NR_MF];')
    start='            NR_MGA(m)\n            for (int k = 0; k < NR_CF; ++k) {'
    assert t.count(start)==1
    at=t.index(start)
    stop=t.index('            // A column of the accumulator is one token',at)
    if 'NR_PORT_IMAGE_QKV' in t:
        stop=t.rfind('#endif\n',at,stop)
        assert stop>at
    t=t[:at]+'#if NR_WINDOW_NATIVE_QKV32\n#include "portable_window_qkv32.glsl"\n#else\n'+t[at:stop]+'#endif // NR_WINDOW_NATIVE_QKV32\n'+t[stop:]
    p.write_text(t)
    for name in ['portable_gemm_core.glsl','portable_window_expand.glsl','portable_window_qkv.glsl','portable_window_residual.glsl','portable_window_middle.glsl','portable_window_qkv32.glsl']:
        (portable/'include'/name).write_bytes((root/'shaders'/name).read_bytes())
