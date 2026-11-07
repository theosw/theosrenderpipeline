"""Prepare reviewed engine sources in the isolated build tree; no upstream scripts."""
from pathlib import Path
import os
import json,re,shutil,sys,subprocess
root=Path(__file__).resolve().parents[1]
bulk=Path(os.environ['TRP_VKNR_BUILD'])
vendor=root/'vendor/mauri/linux'
out=bulk/'engine-src'
for base in ['src/core','shaders/rdna3','shaders/passes']:
 for src in (vendor/base).rglob('*'):
  if not src.is_file():continue
  dst=out/base;dst=dst/src.relative_to(vendor/base);dst.parent.mkdir(parents=True,exist_ok=True)
  dst.write_bytes(src.read_bytes())
portable=out/'shaders/portable'
for src in (vendor/'shaders/rdna3').rglob('*'):
 if not src.is_file():continue
 dst=portable/src.relative_to(vendor/'shaders/rdna3');dst.parent.mkdir(parents=True,exist_ok=True)
 text=src.read_text()
 for token,func in [('NR_FRAG_ACC16','nr_soft_acc16'),('NR_FRAG_E4M3','nr_soft_acc16'),('NR_FRAG_ACC','nr_soft_acc'),('NR_FRAG_A16','nr_soft_operand'),('NR_FRAG_B16','nr_soft_operand'),('NR_FRAG_A','nr_soft_operand'),('NR_FRAG_B','nr_soft_operand')]:
  text=re.sub(r'\b'+token+r'\(',func+'(',text)
 if src.name=='coopmm.glsl':
  at=text.index('// NR_I4:')
  text=text[:at]+'#include "rounding.glsl"\n#include "portable_fragments.glsl"\n'+text[at:]
 # The software operands carry canonical K order; the host marker must agree.
 if src.name=='fswin_t.comp':
  text=text.replace('#define NR_OP_KSWAP 1','#define NR_OP_KSWAP 0')
  # Wide head-split bodies otherwise exceed NVIDIA's advertised 48 KiB
  # workgroup limit after adding standard cooperative-matrix staging.
  old='shared NR_E4M3 lds_x[NR_WIN * NR_C];\nshared NR_E4M3 lds_y[NR_WIN * NR_C];'
  new='''#if NR_C >= 128 && !NR_PORT_SHARED_XY
struct NrPortableExchange { NR_E4M3 x[NR_WIN * NR_C]; NR_E4M3 y[NR_WIN * NR_C]; };
layout(set=0,binding=31,std430) coherent buffer PortableExchange { NrPortableExchange nr_exchange[]; };
#define NR_EXCHANGE_ID (gl_WorkGroupID.x + gl_NumWorkGroups.x*(gl_WorkGroupID.y + gl_NumWorkGroups.y*gl_WorkGroupID.z))
#define lds_x nr_exchange[NR_EXCHANGE_ID].x
#define lds_y nr_exchange[NR_EXCHANGE_ID].y
void nr_exchange_barrier() { memoryBarrierBuffer(); barrier(); }
#define barrier nr_exchange_barrier
#else
shared NR_E4M3 lds_x[NR_WIN * NR_C];
shared NR_E4M3 lds_y[NR_WIN * NR_C];
#endif'''
  assert old in text;text=text.replace(old,new)
 dst.write_text(text)
for name in ['portable_fragments.glsl','rounding.glsl']:
 t=(root/'shaders'/name).read_text().replace('nr_e4m3_','nr_safe_e4m3_')
 t=re.sub(r'^(#define NR_(?:LOAD|STORE)_[^\n]+?) \{([^\n]+)\}$',r'\1 do {\2} while(false)',t,flags=re.M)
 (portable/'include'/name).write_text(t)
# Override later MMA redefinition in the original header.
p=portable/'include/coopmm.glsl';t=p.read_text().replace('acc = coopMatMulAdd(a, b, acc)','acc = nr_port_mma(a, b, acc)');p.write_text(t)
(portable/'include/e4m3_emul.glsl').write_text((root/'shaders/engine_quantization.glsl').read_text())
# Small, reviewable host adaptations. Original copies remain unchanged.
core=out/'src/core'
p=core/'nr_graph.cpp';t=p.read_text();t='#include "memory_model.hpp"\n'+t
needle='bool open_model_pack(const std::string& file, const std::string& root, std::string* why) {'
t=t.replace(needle,needle+'\n    if (nr::standalone::weights) { g_pack.root=root; g_pack.file="@memory"; return true; }')
needle='        const auto it = g_pack.index.find(name);'
t=t.replace(needle,'        if (nr::standalone::weights) { const auto it=nr::standalone::weights->find(name); return it==nr::standalone::weights->end()?std::vector<uint8_t>{}:it->second; }\n'+needle)
t=t.replace('return NR_TCHAIN_TILES &&','return false && NR_TCHAIN_TILES &&')
t=t.replace('runner.inv_barrier = !coherent_act;','runner.inv_barrier = false; // Complete availability/visibility barriers on all vendors.')
t=t.replace('        timer.mark("weights-upload");','        nr::logf("standalone: weights uploaded");\n        timer.mark("weights-upload");')
t=t.replace('            nk.create(ctx,','            nr::logf("standalone: compiling noise-field pipeline");\n            nk.create(ctx,')
t=t.replace('    nrvk::Buffer wgt{};\n    size_t wgt_cap','    nrvk::Buffer wgt{};\n    std::vector<nrvk::Buffer> standalone_owned;\n    size_t wgt_cap')
t=t.replace('    ~GraphArenas() {','    ~GraphArenas() {\n        for(auto& b:standalone_owned){if(b.mapped)vkUnmapMemory(device,b.memory);if(b.handle)vkDestroyBuffer(device,b.handle,nullptr);if(b.memory)vkFreeMemory(device,b.memory,nullptr);}')
t=t.replace('            ctx.fp16_twin[act.handle] = act_e.handle;', '            arenas->standalone_owned.push_back(act_e);arenas->standalone_owned.push_back(wgt_e);\n            ctx.fp16_twin[act.handle] = act_e.handle;')
t=t.replace('        ctx.pin = &arenas->pipelines;', '''        VkDeviceSize scratch_bytes=0;
        for(const auto& d:disp) if(d.kern.rfind("fswin",0)==0) {
            unsigned c=d.kern.find("256")!=std::string::npos?256:d.kern.find("128")!=std::string::npos?128:0;
            if(c)scratch_bytes=std::max(scratch_bytes,VkDeviceSize(d.gx)*d.gy*d.gz*64*c*4);
        }
        if(scratch_bytes){
            VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(ctx.physical,&p);
            if(scratch_bytes>p.limits.maxStorageBufferRange)throw std::runtime_error("portable exchange exceeds storage-buffer range");
            auto b=ctx.buffer(scratch_bytes);ctx.portable_scratch=b.handle;arenas->standalone_owned.push_back(b);
        }
        ctx.pin = &arenas->pipelines;''')
p.write_text(t)
p=core/'nr_runtime.cpp';t=p.read_text()
t=t.replace('            dispatch(cmd, *use, step.gx, step.gy, step.gz, push, step.push_bytes, set);', '''            nr::standalone::Stage diagnostic{};
            if(nr::standalone::stage){
                if(s.disp.size()!=s.steps.size())throw std::runtime_error("Diagnostic dispatch mapping differs from plan");
                const auto& d=s.disp.at(i);const auto& p=*d.s;const int key=key_of(p.block,p.layer);
                diagnostic={uint32_t(i),d.kern.c_str(),p.block,p.layer,p.W,p.H,p.co,s.act.handle,s.act_e.handle,0,0};
                if(s.voff.count(key)&&s.vsize.count(key)){diagnostic.offset=s.voff[key];diagnostic.bytes=s.vsize.at(key);}
                nr::standalone::stage(cmd,diagnostic,true);
            }
            dispatch(cmd, *use, step.gx, step.gy, step.gz, push, step.push_bytes, set);
            if(nr::standalone::stage)nr::standalone::stage(cmd,diagnostic,false);''')
t=t.replace('    if (experimental)   // CC_SMALL_FP8_EXPERIMENTAL:', '    if(!std::getenv("NR_ENABLE_PERSIST") || std::atoi(std::getenv("NR_ENABLE_PERSIST"))!=1)arguments.push_back("--no-persist"); // Portable default: explicit inter-layer barriers.\n    if (experimental)   // CC_SMALL_FP8_EXPERIMENTAL:')
t=t.replace('!std::filesystem::is_regular_file(data / "dlssnr-small.bin", ec)','(!nr::standalone::weights && !std::filesystem::is_regular_file(data / "dlssnr-small.bin", ec))')
t=t.replace('if (!std::filesystem::is_regular_file(pack))','if (!nr::standalone::weights && !std::filesystem::is_regular_file(pack))')
p.write_text(t)
p=core/'telemetry.hpp';t=p.read_text().replace('#include <dirent.h>','#ifndef _WIN32\n#include <dirent.h>\n#endif')
t=t.replace('        for (int card = 0; card < 8; ++card) {','#ifndef _WIN32\n        for (int card = 0; card < 8; ++card) {').replace('        return {};\n    }',' #endif\n        return {};\n    }',1);p.write_text(t)
p=core/'nrvk.hpp';t=p.read_text().replace('pipeline_robustness = prf.pipelineRobustness;','pipeline_robustness = false; // Standalone device does not enable this optional feature.')
t=t.replace('    std::map<VkBuffer, VkBuffer> fp16_twin;','    std::map<VkBuffer, VkBuffer> fp16_twin;\n    VkBuffer portable_scratch{};')
t=t.replace('        for (const auto& alias : aliases) alias_bindings.push_back(alias.first);','        if(declared.count(31)){if(!ctx.portable_scratch)throw std::runtime_error("missing portable exchange buffer");aliases.push_back({31,ctx.portable_scratch});}\n        for (const auto& alias : aliases) alias_bindings.push_back(alias.first);')
t=t.replace('NRVK_CHECK(vkCreateComputePipelines(', 'std::fprintf(stderr,"standalone: driver pipeline compile %s\\n",spirv_path.c_str());\n        NRVK_CHECK(vkCreateComputePipelines(')
t=t.replace('#pragma once','#pragma once\n#include <chrono>')
t=t.replace('cpi.layout = out.layout;', '''cpi.layout = out.layout;
        const char* opt=std::getenv("NR_DRIVER_OPT");
        if(opt && *opt=='0')cpi.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;''')
t=t.replace('        NRVK_CHECK(vkCreateComputePipelines(device, ctx.pipeline_cache, 1, &cpi, nullptr, &out.pipeline));', '''        const auto compile_start=std::chrono::steady_clock::now();
        NRVK_CHECK(vkCreateComputePipelines(device, ctx.pipeline_cache, 1, &cpi, nullptr, &out.pipeline));
        std::fprintf(stderr,"pipeline_ms %.3f %s\\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-compile_start).count(),spirv_path.c_str());''')
# The portable shaders turn off shared-memory aliasing. RDNA3 support will require this feature in the owner.
t=t.replace('if (!wml.workgroupMemoryExplicitLayout || !wml.workgroupMemoryExplicitLayout8BitAccess ||','if (!NR_STANDALONE_PORTABLE && (!wml.workgroupMemoryExplicitLayout || !wml.workgroupMemoryExplicitLayout8BitAccess ||').replace('!wml.workgroupMemoryExplicitLayout16BitAccess)\n            return no','!wml.workgroupMemoryExplicitLayout16BitAccess))\n            return no')
p.write_text(t)
print('Prepared reviewed engine and portable compatibility sources:',out)

# Reproducible coordinator-selected transformations; original vendor files stay untouched.
import runpy
runpy.run_path(str(root/"tools/prepare-deep.py"))["apply"](portable,root)
for name in ["coord-cache.py","coord-pipelines.py","coord-window-selector.py"]:
 runpy.run_path(str(root/"tools"/name))

# Decoder snapshot precedes window-only edits to the shared body.
runpy.run_path(str(root/"tools/prepare-decoder.py"))["apply"](portable,root)

# Window lane: reuse deep accumulator core for owned window linears.
runpy.run_path(str(root/"tools/prepare-window.py"))["apply"](portable,root)

# Exact global attention; no temporal reuse.
runpy.run_path(str(root/"tools/prepare-global.py"))["apply"](portable,root)

# TRP integration: immutable precision/schedule policy and private cache path.
p=core/'nr_runtime.hpp';t=p.read_text().replace('struct RuntimeConfig {','struct RuntimeConfig {\n    std::string cache_file;');p.write_text(t)
p=core/'nr_runtime.cpp';t=p.read_text()
t=t.replace('(installed ? data / "pipeline.cache" : root / "dlssnr-amd.pipelinecache").string()', 'config.cache_file')
t=t.replace('if(!std::getenv("NR_ENABLE_PERSIST") || std::atoi(std::getenv("NR_ENABLE_PERSIST"))!=1)', 'if(true)')
p.write_text(t)
p=core/'nr_gpu_completion.hpp';t=p.read_text()
a=t.index('        return submissions.load() - touched_at')
b=t.index('\n    }',a)
t=t[:a]+'        return false; // TRP requires actual fence retirement, never age/submission heuristics.'+t[b:]
t=t.replace('if (ring.size() >= 8192) return ring[index];','if (ring.size() >= 8192) throw std::runtime_error("TRP NR descriptor retirement exhausted");')
p.write_text(t)

# If construction fails after an initialization submit, partial graph resources
# must outlive that submit. The DLL retains the corresponding Session/device.
p=core/'nr_runtime.cpp';t=p.read_text()
t=t.replace(': impl_(std::make_unique<Impl>()) {', ': impl_(std::make_unique<Impl>()) {\n    try {', 1)
t=t.replace('\n}\n\nRuntime::~Runtime() = default;', '\n    } catch (...) { impl_.release(); throw; }\n}\n\nRuntime::~Runtime() = default;', 1)
p.write_text(t)
