"""Own portable GEMM source transformation; runs only on reviewed prepared copies."""
from pathlib import Path
import os

def apply(portable,root):
 p=portable/'gemm1x1.comp';t=p.read_text()
 start='    NR_FRAG_ACC acc[NR_MFRAG][NR_NFRAG];\n'
 end='#endif  // NR_XPIPE\n'
 assert t.count(start)==1 and t.count(end)==1
 t=t.replace(start,start+'#if defined(NR_PORTABLE_GEMM_CORE)\n#include "portable_gemm_core.glsl"\n#else\n')
 t=t.replace(end,end+'#endif // NR_PORTABLE_GEMM_CORE\n')
 p.write_text(t)
 (portable/'include/portable_gemm_core.glsl').write_bytes((root/'shaders/portable_gemm_core.glsl').read_bytes())

 core=portable.parents[1]/'src/core'
 p=core/'nrvk.hpp';t=p.read_text();t='#include "portable_deep_select.hpp"\n'+t
 needle='    void make_pipeline(Context& ctx, const std::string& spirv_path, uint32_t push_bytes,\n                       const std::vector<VkDescriptorSetLayoutBinding>& lb, SharedPipeline& out) {'
 assert t.count(needle)==1
 t=t.replace(needle,needle.replace('const std::string& spirv_path','const std::string& requested_path')+'\n        const std::string spirv_path=nrtest::deep_shape_path(ctx.instance,ctx.physical,requested_path);')
 p.write_text(t)
