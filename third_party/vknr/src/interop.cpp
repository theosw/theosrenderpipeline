// Standalone implementation of the MIT mochizuki bridge's Win32 import/run_between contract.
// The original reviewed bridge is retained separately; no host hooks or injection code are used.
#include "interop.hpp"
#include <cstring>
#include <stdexcept>

namespace nrtest {
void Interop::hr(HRESULT r,const char* why){if(FAILED(r))throw std::runtime_error(std::string(why)+" HRESULT="+std::to_string(uint32_t(r)));}
Interop::Com<ID3D12Resource> Interop::buffer(UINT64 bytes,D3D12_HEAP_TYPE type){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=type;
 D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 Com<ID3D12Resource> r;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"D3D12 staging resource");return r;
}
void Interop::make_texture(Texture& t,DXGI_FORMAT format,VkFormat vf,unsigned bpp){
 t.format=format;t.bpp=bpp;
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_SHARED,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&t.resource)),"D3D12 shared texture");
 HANDLE handle{};hr(device->CreateSharedHandle(t.resource.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"D3D12 texture handle");
 try {
  auto& im=t.image;im.w=width;im.h=height;im.format=vf;im.layout=VK_IMAGE_LAYOUT_GENERAL;
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.pNext=&external;ii.imageType=VK_IMAGE_TYPE_2D;ii.format=vf;ii.extent={width,height,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  NRVK_CHECK(vkCreateImage(ctx.device,&ii,nullptr,&im.handle));
  VkMemoryRequirements req{};vkGetImageMemoryRequirements(ctx.device,im.handle,&req);
  auto props=reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(vkGetDeviceProcAddr(ctx.device,"vkGetMemoryWin32HandlePropertiesKHR"));if(!props)throw std::runtime_error("Win32 external memory unavailable");
  VkMemoryWin32HandlePropertiesKHR m{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};NRVK_CHECK(props(ctx.device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,handle,&m));
  unsigned type=ctx.mem.memoryTypeCount;
  for(unsigned i=0;i<ctx.mem.memoryTypeCount;++i)if((req.memoryTypeBits&m.memoryTypeBits&(1u<<i))&&(ctx.mem.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
  if(type==ctx.mem.memoryTypeCount)throw std::runtime_error("No matching external image memory type");
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=im.handle;
  VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};import.pNext=&dedicated;import.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;import.handle=handle;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.pNext=&import;ai.allocationSize=req.size;ai.memoryTypeIndex=type;NRVK_CHECK(vkAllocateMemory(ctx.device,&ai,nullptr,&im.memory));NRVK_CHECK(vkBindImageMemory(ctx.device,im.handle,im.memory,0));
 }catch(...){CloseHandle(handle);throw;}CloseHandle(handle);
}
Interop::Interop(nrvk::Context& c,unsigned w,unsigned h,unsigned frames):ctx(c),width(w),height(h),jobs(frames){
 VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&id;vkGetPhysicalDeviceProperties2(c.physical,&p);
 if(!id.deviceLUIDValid)throw std::runtime_error("Vulkan device has no Windows adapter LUID");
 LUID luid{};std::memcpy(&luid,id.deviceLUID,sizeof luid);
 Com<IDXGIFactory4> factory;Com<IDXGIAdapter> adapter;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");hr(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"Match D3D12 adapter LUID");hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"D3D12 device");
 D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"D3D12 queue");hr(queue->GetTimestampFrequency(&frequency),"D3D12 timestamp frequency");
 hr(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&timeline)),"D3D12 shared timeline");hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&drain)),"D3D12 final fence");
 HANDLE handle{};hr(device->CreateSharedHandle(timeline.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"D3D12 fence handle");
 VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};st.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
 VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&st;NRVK_CHECK(vkCreateSemaphore(ctx.device,&si,nullptr,&semaphore));
 auto import=reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(ctx.device,"vkImportSemaphoreWin32HandleKHR"));if(!import){CloseHandle(handle);throw std::runtime_error("Win32 timeline import unavailable");}
 VkImportSemaphoreWin32HandleInfoKHR ii{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};ii.semaphore=semaphore;ii.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;ii.handle=handle;auto imported=import(ctx.device,&ii);CloseHandle(handle);NRVK_CHECK(imported);
 make_texture(textures[0],DXGI_FORMAT_R16G16B16A16_FLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,8);
 make_texture(textures[1],DXGI_FORMAT_R32G32_FLOAT,VK_FORMAT_R32G32_SFLOAT,8);
 make_texture(textures[2],DXGI_FORMAT_R32_FLOAT,VK_FORMAT_R32_SFLOAT,4);
 make_texture(textures[3],DXGI_FORMAT_R16G16B16A16_FLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,8);
 ctx.one_shot([&](VkCommandBuffer cmd){ownership(cmd,false,true);}); // Startup only, before D3D12 writes.
 VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=ctx.family;NRVK_CHECK(vkCreateCommandPool(ctx.device,&pi,nullptr,&pool));
 D3D12_QUERY_HEAP_DESC qh{};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=frames*4;hr(device->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)),"D3D12 timestamps");timestamp_readback=buffer(UINT64(frames)*4*8,D3D12_HEAP_TYPE_READBACK);
}
void Interop::ownership(VkCommandBuffer cmd,bool acquire,bool initial){
 std::array<VkImageMemoryBarrier,4> barriers{};
 for(unsigned i=0;i<4;++i){auto& b=barriers[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=textures[i].image.handle;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.oldLayout=initial?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcQueueFamilyIndex=acquire?VK_QUEUE_FAMILY_EXTERNAL:ctx.family;b.dstQueueFamilyIndex=acquire?ctx.family:VK_QUEUE_FAMILY_EXTERNAL;b.srcAccessMask=acquire||initial?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=acquire?VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT:0;}
 vkCmdPipelineBarrier(cmd,acquire||initial?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,acquire?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,4,barriers.data());
}
static void transition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cmd->ResourceBarrier(1,&b);}
void Interop::upload(Job& j,Texture& t,const void* data){
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 size{};auto d=t.resource->GetDesc();device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&size);auto staging=buffer(size,D3D12_HEAP_TYPE_UPLOAD);
 void* mapped{};D3D12_RANGE empty{0,0};hr(staging->Map(0,&empty,&mapped),"Map upload");for(unsigned y=0;y<height;++y)std::memcpy(static_cast<char*>(mapped)+fp.Offset+size_t(y)*fp.Footprint.RowPitch,static_cast<const char*>(data)+size_t(y)*width*t.bpp,size_t(width)*t.bpp);staging->Unmap(0,nullptr);
 D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=t.resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 transition(j.before.Get(),t.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);j.before->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(j.before.Get(),t.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);j.uploads.push_back(staging);
}
VkCommandBuffer Interop::begin(unsigned f,const uint16_t* colour,const float* motion,const float* depth){
 auto& j=jobs.at(f);hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&j.before_allocator)),"D3D12 allocator");hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,j.before_allocator.Get(),nullptr,IID_PPV_ARGS(&j.before)),"D3D12 list");
 j.before->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f*4);upload(j,textures[0],colour);if(f==0){upload(j,textures[1],motion);upload(j,textures[2],depth);}j.before->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f*4+1);j.before->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f*4,2,timestamp_readback.Get(),UINT64(f)*32);hr(j.before->Close(),"Close D3D12 input list");
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.commandBufferCount=1;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;NRVK_CHECK(vkAllocateCommandBuffers(ctx.device,&ai,&j.command));VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;NRVK_CHECK(vkBeginCommandBuffer(j.command,&bi));return j.command;
}
void Interop::submit(unsigned f,bool capture){
 auto& j=jobs.at(f);NRVK_CHECK(vkEndCommandBuffer(j.command));
 hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&j.after_allocator)),"D3D12 output allocator");hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,j.after_allocator.Get(),nullptr,IID_PPV_ARGS(&j.after)),"D3D12 output list");j.after->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f*4+2);
 if(capture){auto& t=textures[3];auto d=t.resource->GetDesc();UINT64 size{};device->GetCopyableFootprints(&d,0,1,0,&j.footprint,nullptr,nullptr,&size);j.readback=buffer(size,D3D12_HEAP_TYPE_READBACK);D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=t.resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=j.readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=j.footprint;transition(j.after.Get(),t.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);j.after->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(j.after.Get(),t.resource.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);}
 j.after->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f*4+3);j.after->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f*4+2,2,timestamp_readback.Get(),UINT64(f)*32+16);hr(j.after->Close(),"Close D3D12 output list");
 ID3D12CommandList* before=j.before.Get();queue->ExecuteCommandLists(1,&before);const uint64_t ready=uint64_t(f)*2+1,done=ready+1;hr(queue->Signal(timeline.Get(),ready),"Signal D3D12 ready");
 VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};ts.waitSemaphoreValueCount=ts.signalSemaphoreValueCount=1;ts.pWaitSemaphoreValues=&ready;ts.pSignalSemaphoreValues=&done;VkPipelineStageFlags wait=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.pNext=&ts;si.waitSemaphoreCount=si.signalSemaphoreCount=1;si.pWaitSemaphores=si.pSignalSemaphores=&semaphore;si.pWaitDstStageMask=&wait;si.commandBufferCount=1;si.pCommandBuffers=&j.command;NRVK_CHECK(vkQueueSubmit(ctx.queue,1,&si,VK_NULL_HANDLE));
 hr(queue->Wait(timeline.Get(),done),"D3D12 GPU wait for Vulkan");ID3D12CommandList* after=j.after.Get();queue->ExecuteCommandLists(1,&after);
}
void Interop::finish(){if(finished)return;hr(queue->Signal(drain.Get(),1),"Final drain signal");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Create final drain event");auto result=drain->SetEventOnCompletion(1,event);if(FAILED(result)){CloseHandle(event);hr(result,"Set drain event");}auto wait=WaitForSingleObject(event,60000);CloseHandle(event);if(wait!=WAIT_OBJECT_0)throw std::runtime_error("Final D3D12 drain timed out");NRVK_CHECK(vkQueueWaitIdle(ctx.queue));finished=true;}
void Interop::read(unsigned f,std::vector<uint16_t>& data){auto& j=jobs.at(f);if(!finished||!j.readback)throw std::runtime_error("Read before final drain or missing capture");void* mapped{};hr(j.readback->Map(0,nullptr,&mapped),"Map D3D12 output");data.resize(size_t(width)*height*4);for(unsigned y=0;y<height;++y)std::memcpy(data.data()+size_t(y)*width*4,static_cast<char*>(mapped)+j.footprint.Offset+size_t(y)*j.footprint.Footprint.RowPitch,size_t(width)*8);D3D12_RANGE empty{0,0};j.readback->Unmap(0,&empty);}
std::array<double,3> Interop::copy_times(unsigned f){uint64_t* ticks{};hr(timestamp_readback->Map(0,nullptr,reinterpret_cast<void**>(&ticks)),"Map D3D12 timestamps");std::array<double,3> result{double(ticks[f*4+1]-ticks[f*4])*1000.0/frequency,double(ticks[f*4+3]-ticks[f*4+2])*1000.0/frequency,double(ticks[f*4+2]-ticks[f*4+1])*1000.0/frequency};D3D12_RANGE empty{0,0};timestamp_readback->Unmap(0,&empty);return result;}
Interop::~Interop(){if(queue&&!finished){try{finish();}catch(...){}}if(ctx.device){vkQueueWaitIdle(ctx.queue);for(auto& t:textures)ctx.destroy(t.image);if(pool)vkDestroyCommandPool(ctx.device,pool,nullptr);if(semaphore)vkDestroySemaphore(ctx.device,semaphore,nullptr);}}
}
