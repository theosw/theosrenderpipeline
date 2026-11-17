#include "interop.hpp"
#include "engine.hpp"
#include "weights.hpp"
#include "memory_model.hpp"
#include "nr_runtime.hpp"
#include "tinlayout.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace nrtest {
std::string json_quote(const std::string&);
std::string run_engine(VkInstance inst,VkPhysicalDevice phys,VkDevice dev,VkQueue queue,uint32_t family,const EngineRequest& req) {
 if(!req.width||!req.height||req.width>8192||req.height>8192||!req.frames||req.frames>128||req.warmup>=req.frames||req.capture>req.frames)throw std::runtime_error("Invalid extent/frame/warmup/capture count");
 if(req.handoff && req.frames>64)throw std::runtime_error("Handoff batch limited to 64 frames to preserve runtime descriptor lifetimes");
 auto weights=load_small_weights(req.dll);const size_t pixels=size_t(req.width)*req.height;
 std::ifstream file(req.input,std::ios::binary|std::ios::ate);if(!file||file.tellg()!=std::streamoff(pixels*16))throw std::runtime_error("Input must be RGBA32F of requested extent");
 file.seekg(0);std::vector<float> input(pixels*4);file.read(reinterpret_cast<char*>(input.data()),std::streamsize(input.size()*4));for(float v:input)if(!std::isfinite(v))throw std::runtime_error("Nonfinite input");
 nrvk::Context ctx;std::string why;if(!ctx.adopt(inst,phys,dev,queue,family,&why))throw std::runtime_error(why);
 std::unique_ptr<Interop> interop;nrvk::Context::Image colour{},motion{},depth{},target{};
 std::vector<float> mv(pixels*2,0),z(pixels,.5f);for(size_t i=0;i<pixels;++i)mv[2*i]=-float(req.shift)/req.width;
 if(req.handoff){interop=std::make_unique<Interop>(ctx,req.width,req.height,req.frames);colour=interop->colour();motion=interop->motion();depth=interop->depth();target=interop->output();}
 else{colour=ctx.image(req.width,req.height,VK_FORMAT_R16G16B16A16_SFLOAT,true);motion=ctx.image(req.width,req.height,VK_FORMAT_R32G32_SFLOAT,true);depth=ctx.image(req.width,req.height,VK_FORMAT_R32_SFLOAT,true);ctx.upload(motion,mv.data(),mv.size()*4);ctx.upload(depth,z.data(),z.size()*4);}
 nr::HostDevice host;host.instance=inst;host.physical=phys;host.device=dev;host.queue=queue;host.queue_family=family;
 nr::RuntimeConfig config;config.root=req.root.u8string();config.width=req.width;config.height=req.height;config.network=1;config.colour_format=colour.format;
 nr::TemporalConfig temporal;temporal.enable=true;temporal.shaders=(req.root/"dlssnr-amd/shaders/temporal").u8string();
 std::unique_ptr<nr::Runtime> runtime;nr::standalone::weights=&weights.entries;
 const auto init_start=std::chrono::steady_clock::now();
 try{runtime=std::make_unique<nr::Runtime>(host,config,nr::ControlMaskConfig{},temporal);}catch(...){nr::standalone::weights=nullptr;throw;}nr::standalone::weights=nullptr;weights.entries.clear();
 const double initialization_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-init_start).count();
 std::cerr<<"initialization_ms "<<initialization_ms<<"\n";
 if(runtime->network()!=nr::NetworkModel::Small)throw std::runtime_error("Unexpected model");
 nr::Controls controls;controls.intensity=controls.local_tone=controls.local_structure=controls.skin_structure=1;controls.automatic_mask=false;controls.style=0;
 auto frame_of=[](const nrvk::Context::Image& im){nr::ColourFrame f;f.image=im.handle;f.format=im.format;f.width=im.w;f.height=im.h;f.before=f.after=im.layout;f.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;return f;};
 nr::EngineFrame frame;frame.colour=frame_of(colour);frame.motion=frame_of(motion);frame.depth=frame_of(depth);frame.feature=1;if(interop)frame.target=frame_of(target);frame.motion_scale_x=frame.motion_scale_y=1;
 constexpr unsigned max_stages=256;const bool instrument=req.stage_timestamps||req.layers;const unsigned stride=instrument?520:4;VkQueryPool query{};VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=stride*req.frames;NRVK_CHECK(vkCreateQueryPool(dev,&qi,nullptr,&query));
 std::vector<uint16_t> upload(pixels*4),download(pixels*4);
 struct StageMeta{std::string kernel;int block,layer,w,h,c;};std::vector<StageMeta> stages;
 struct Capture{nrvk::Buffer buffer;unsigned index;bool expanded;VkDeviceSize offset;};std::vector<Capture> layers;
 struct HookGuard{~HookGuard(){nr::standalone::stage={};}} hook_guard;
 std::vector<nr::EngineResult> results(req.frames);std::vector<unsigned> stage_counts(req.frames);
 bool finite=true,nonblack=false;float maximum=0,minimum=1e30f;std::filesystem::create_directories(req.output.parent_path());
 auto capture_output=[&](unsigned f){std::vector<float> rgba(pixels*4);for(size_t i=0;i<rgba.size();++i){rgba[i]=tin::f16_to_f(download[i]);finite&=std::isfinite(rgba[i]);if(i%4<3){nonblack|=rgba[i]>.001f;minimum=std::min(minimum,rgba[i]);maximum=std::max(maximum,rgba[i]);}}if(f+req.capture>=req.frames){std::ofstream out(req.output.u8string()+".frame"+std::to_string(f)+".f32",std::ios::binary);out.write(reinterpret_cast<char*>(rgba.data()),std::streamsize(rgba.size()*4));if(!out)throw std::runtime_error("Output capture failed");}};
 for(unsigned f=0;f<req.frames;++f){
  for(unsigned y=0;y<req.height;++y)for(unsigned x=0;x<req.width;++x){const int sx=std::clamp(int(x)-int(f)*req.shift,0,int(req.width)-1);for(unsigned c=0;c<4;++c)upload[(size_t(y)*req.width+x)*4+c]=tin::f_to_f16(input[(size_t(y)*req.width+sx)*4+c]);}
  if(!interop)ctx.upload(colour,upload.data(),upload.size()*2);frame.reset=f==0;const unsigned base=f*stride;
  if(instrument)nr::standalone::stage=[&](VkCommandBuffer cmd,const nr::standalone::Stage& s,bool before){
   if(s.index>=max_stages)throw std::runtime_error("Stage capacity exceeded");vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,base+8+s.index*2+(before?0:1));
   if(before){if(f==0)stages.push_back({s.kernel,s.block,s.layer,s.width,s.height,s.channels});stage_counts[f]=std::max(stage_counts[f],s.index+1);return;}
   if(req.layers&&f+1==req.frames&&s.bytes){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&b,0,nullptr,0,nullptr);
    for(bool expanded:{false,true}){if(expanded&&!s.expanded)continue;const VkDeviceSize scale=expanded?2:1;auto buf=ctx.buffer(s.bytes*scale,true);VkBufferCopy copy{s.offset*scale,0,s.bytes*scale};vkCmdCopyBuffer(cmd,expanded?s.expanded:s.raw,buf.handle,1,&copy);layers.push_back({buf,s.index,expanded,s.offset*scale});}
    b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,nullptr,0,nullptr);}
  };
  auto record=[&](VkCommandBuffer cmd){vkCmdResetQueryPool(cmd,query,base,stride);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,query,base);if(interop)interop->acquire(cmd);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,base+1);results[f]=runtime->record_engine(cmd,frame,controls);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,base+2);if(interop)interop->release(cmd);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,base+3);};
  const bool capture=f+req.capture>=req.frames||f+1==req.frames;
  if(interop){auto cmd=interop->begin(f,upload.data(),mv.data(),z.data());record(cmd);interop->submit(f,capture);}else{ctx.one_shot(record);if(capture){ctx.download(colour,download.data(),download.size()*2);capture_output(f);}}
  if(!results[f].frame.applied)throw std::runtime_error("Full network not applied");std::cout<<"frame "<<f<<" recorded, "<<stage_counts[f]<<" instrumented dispatches\n"<<std::flush;
 }
 nr::standalone::stage={};if(interop)interop->finish();
 std::vector<double> times;std::ostringstream samples,stage_json;std::vector<std::vector<double>> stage_times(stages.size());
 auto quantile=[](std::vector<double> v,double p){std::sort(v.begin(),v.end());return v[std::min(v.size()-1,size_t(std::ceil(p*v.size())-1))];};
 for(unsigned f=0;f<req.frames;++f){uint64_t ticks[4]{};NRVK_CHECK(vkGetQueryPoolResults(dev,query,f*stride,4,sizeof ticks,ticks,8,VK_QUERY_RESULT_64_BIT));const double scale=ctx.timestamp_period*1e-6,ms=double(ticks[2]-ticks[1])*scale;if(f>=req.warmup)times.push_back(ms);
  if(f)samples<<',';samples<<std::setprecision(10)<<"{\"frame\":"<<f<<",\"gpu_ms\":"<<ms<<",\"dispatches\":"<<results[f].frame.network_dispatches<<",\"history\":"<<(results[f].history_consumed?"true":"false");
  if(interop){auto c=interop->copy_times(f);samples<<",\"acquire_ms\":"<<double(ticks[1]-ticks[0])*scale<<",\"release_ms\":"<<double(ticks[3]-ticks[2])*scale<<",\"d3d12_input_copy_ms\":"<<c[0]<<",\"d3d12_output_copy_ms\":"<<c[1]<<",\"d3d12_bridge_span_ms\":"<<c[2]<<",\"handoff_residual_ms\":"<<c[2]-ms;if(f+req.capture>=req.frames||f+1==req.frames){interop->read(f,download);capture_output(f);}}
  std::vector<uint64_t> lt(size_t(stage_counts[f])*2);if(stage_counts[f])NRVK_CHECK(vkGetQueryPoolResults(dev,query,f*stride+8,stage_counts[f]*2,lt.size()*8,lt.data(),8,VK_QUERY_RESULT_64_BIT));samples<<",\"stage_ms\":[";for(unsigned i=0;i<stage_counts[f];++i){double t=double(lt[i*2+1]-lt[i*2])*scale;if(i)samples<<',';samples<<t;if(f>=req.warmup)stage_times[i].push_back(t);}samples<<"]}";
 }
 std::ostringstream captures;bool comma=false;for(auto& c:layers){auto name="stage"+std::to_string(c.index)+(c.expanded?".expanded-fp16.bin":".raw.bin");auto folder=std::filesystem::path(req.output.u8string()+".layers");std::filesystem::create_directories(folder);std::ofstream out(folder/name,std::ios::binary);out.write(static_cast<const char*>(c.buffer.mapped),std::streamsize(c.buffer.bytes));if(!out)throw std::runtime_error("Activation capture write failed");if(comma)captures<<',';comma=true;captures<<"{\"file\":"<<json_quote(name)<<",\"stage\":"<<c.index<<",\"arena_offset\":"<<c.offset<<",\"bytes\":"<<c.buffer.bytes<<",\"expanded\":"<<(c.expanded?"true":"false")<<'}';ctx.destroy(c.buffer);}
 for(unsigned i=0;i<stages.size();++i){auto& s=stages[i];if(i)stage_json<<',';stage_json<<"{\"index\":"<<i<<",\"kernel\":"<<json_quote(s.kernel)<<",\"block\":"<<s.block<<",\"layer\":"<<s.layer<<",\"plan_width\":"<<s.w<<",\"plan_height\":"<<s.h<<",\"channels\":"<<s.c<<",\"initialization_ms\":"<<initialization_ms<<",\"median_ms\":"<<quantile(stage_times[i],.5)<<",\"p95_ms\":"<<quantile(stage_times[i],.95)<<'}';}
 VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(phys,&properties);runtime.reset();vkDestroyQueryPool(dev,query,nullptr);if(interop)interop.reset();else{ctx.destroy(depth);ctx.destroy(motion);ctx.destroy(colour);}ctx.destroy();
 std::ostringstream o;o<<std::setprecision(10)<<"{\"backend\":"<<json_quote(req.backend)<<",\"device_label\":"<<json_quote(std::string(properties.deviceName)+", "+req.backend+" backend")<<",\"width\":"<<req.width<<",\"height\":"<<req.height<<",\"frames\":"<<req.frames<<",\"warmup\":"<<req.warmup<<",\"shift_px\":"<<req.shift<<",\"handoff\":"<<(req.handoff?"true":"false")<<",\"steady_state_network_cpu_waits\":"<<(req.handoff?0:req.frames)<<",\"stage_timestamps\":"<<(instrument?"true":"false")<<",\"layer_capture\":"<<(req.layers?"true":"false")<<",\"finite\":"<<(finite?"true":"false")<<",\"nonblack\":"<<(nonblack?"true":"false")<<",\"minimum\":"<<minimum<<",\"maximum\":"<<maximum<<",\"initialization_ms\":"<<initialization_ms<<",\"median_ms\":"<<quantile(times,.5)<<",\"p95_ms\":"<<quantile(times,.95)<<",\"stages\":["<<stage_json.str()<<"],\"layer_captures\":["<<captures.str()<<"],\"samples\":["<<samples.str()<<"]}\n";
 if(!finite||!nonblack)throw std::runtime_error("Nonfinite or black output");return o.str();
}
}
