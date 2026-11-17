"""Reviewed window-lane shared-memory variant selection, before reflection."""
from pathlib import Path
import os
p=Path(os.environ['TRP_VKNR_BUILD'])/'engine-src/src/core/nrvk.hpp';t=p.read_text()
t=t.replace('void create(Context& ctx, const std::string& spirv_path,','void create(Context& ctx, std::string spirv_path,')
needle='        device = ctx.device;\n        push_range = push_bytes;'
assert t.count(needle)==1
t=t.replace(needle,'''        device = ctx.device;
        const bool window128 = spirv_path.find("g_fswin128.spv") != std::string::npos ||
            spirv_path.find("g_fswinds128.spv") != std::string::npos ||
            spirv_path.find("g_fswindsp128.spv") != std::string::npos ||
            spirv_path.find("g_fswinfusedup128.spv") != std::string::npos;
        if(window128){
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(ctx.physical,&properties);
            const std::string alternate=spirv_path+".shared48.spv";
            if(properties.limits.maxComputeSharedMemorySize>=48u*1024u &&
               std::ifstream(alternate,std::ios::binary).good())spirv_path=alternate;
        }
        push_range = push_bytes;''')
p.write_text(t)
print('Prepared queried C128 shared-memory selector')
