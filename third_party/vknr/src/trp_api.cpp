// TRP-owned submission adapter for the MIT coord-r005 engine.
#define VK_USE_PLATFORM_WIN32_KHR
#include "device_context.hpp"
#include "weights.hpp"
#include "memory_model.hpp"
#include "nr_runtime.hpp"
#include "nr_gpu_completion.hpp"
#include "nrvk.hpp"
#include <array>
#include <mutex>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <map>

namespace {
std::mutex apiMutex; // also serializes the engine's in-memory weight construction hook
thread_local std::string lastError;
std::map<std::uint64_t,vknr::Capability> admissions; // protected by apiMutex; survives feature retirement
std::uint64_t adapter_key(ID3D12Device* device) {
    const auto luid=device->GetAdapterLuid();
    return (std::uint64_t(std::uint32_t(luid.HighPart))<<32)|luid.LowPart;
}
struct Session {
    vknr::DeviceOwner owner;
    nrvk::Context ctx;
    std::unique_ptr<nr::Runtime> runtime;
    std::array<nrvk::Context::Image,4> images{};
    VkSemaphore semaphore{};
    VkCommandPool pool{};
    VkQueryPool queries{};
    struct Slot { VkCommandBuffer command{}; VkFence fence{}; bool submitted{}; std::uint64_t done{},timedDone{}; };
    std::array<Slot,vknr::Slots> slots{};
    vknr::Frame prepared{};
    bool pending{}, poisoned{}, uncertain{};
    std::uint64_t lastDone{}, timedDone{};
    double milliseconds{-1};
    std::uint64_t completedDone{},samples{};
    double totalMilliseconds{};
    std::string error;
    void harvest(unsigned index) {
        auto& slot=slots[index];uint64_t ticks[2]{};
        completedDone=(std::max)(completedDone,slot.done);
        if(slot.done>slot.timedDone&&vkGetQueryPoolResults(ctx.device,queries,index*2,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
            VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(ctx.physical,&props);
            const auto ms=double(ticks[1]-ticks[0])*props.limits.timestampPeriod/1e6;
            totalMilliseconds+=ms;++samples;slot.timedDone=slot.done;
            if(slot.done>timedDone){milliseconds=ms;timedDone=slot.done;}
        }
    }
    void ownership(VkCommandBuffer cmd,bool acquire,bool initial=false) {
        std::array<VkImageMemoryBarrier,4> barriers{};
        for(unsigned i=0;i<4;++i) {
            auto& b=barriers[i]; b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.image=images[i].handle;
            b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            b.oldLayout=initial?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL; b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex=acquire?VK_QUEUE_FAMILY_EXTERNAL:ctx.family;
            b.dstQueueFamilyIndex=acquire?ctx.family:VK_QUEUE_FAMILY_EXTERNAL;
            b.srcAccessMask=acquire||initial?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask=acquire?VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT:0;
        }
        vkCmdPipelineBarrier(cmd,acquire||initial?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            acquire?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,4,barriers.data());
    }
    void import_image(unsigned i,const vknr::SharedImage& src) {
        static constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
        static constexpr VkFormat vkFormats[]={VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT};
        if(!src.handle||src.format!=formats[i]||!src.width||!src.height||src.width>8192||src.height>8192)
            throw std::runtime_error("Invalid shared Vulkan NR texture contract");
        auto& im=images[i]; im.w=src.width; im.h=src.height; im.format=vkFormats[i]; im.layout=VK_IMAGE_LAYOUT_GENERAL;
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.pNext=&ext; info.imageType=VK_IMAGE_TYPE_2D;
        info.format=im.format; info.extent={im.w,im.h,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
        info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        NRVK_CHECK(vkCreateImage(ctx.device,&info,nullptr,&im.handle));
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(ctx.device,im.handle,&req);
        auto properties=reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(vkGetDeviceProcAddr(ctx.device,"vkGetMemoryWin32HandlePropertiesKHR"));
        if(!properties)throw std::runtime_error("Missing Win32 image import function");
        VkMemoryWin32HandlePropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        NRVK_CHECK(properties(ctx.device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,src.handle,&props));
        unsigned type=ctx.mem.memoryTypeCount;
        for(unsigned j=0;j<ctx.mem.memoryTypeCount;++j)
            if((req.memoryTypeBits&props.memoryTypeBits&(1u<<j))&&(ctx.mem.memoryTypes[j].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=j;break;}
        if(type==ctx.mem.memoryTypeCount)throw std::runtime_error("No D3D12/Vulkan shared memory type");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image=im.handle;
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR}; import.pNext=&dedicated;
        import.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT; import.handle=src.handle;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.pNext=&import; alloc.allocationSize=req.size; alloc.memoryTypeIndex=type;
        NRVK_CHECK(vkAllocateMemory(ctx.device,&alloc,nullptr,&im.memory)); NRVK_CHECK(vkBindImageMemory(ctx.device,im.handle,im.memory,0));
    }
    bool retired() {
        if(uncertain)return false;
        for(auto& s:slots) if(s.submitted) {
            const auto result=vkGetFenceStatus(ctx.device,s.fence);
            if(result==VK_NOT_READY)return false;
            if(result!=VK_SUCCESS)throw std::runtime_error("Vulkan fence retirement is unconfirmed");
        }
        return true;
    }
    ~Session() {
        runtime.reset();
        if(!ctx.device)return;
        for(auto& s:slots) { if(s.command)nr::gpu::reset(s.command); if(s.fence)vkDestroyFence(ctx.device,s.fence,nullptr); }
        if(queries)vkDestroyQueryPool(ctx.device,queries,nullptr);
        if(pool)vkDestroyCommandPool(ctx.device,pool,nullptr);
        if(semaphore)vkDestroySemaphore(ctx.device,semaphore,nullptr);
        for(auto& im:images)ctx.destroy(im);
    }
};
nr::ColourFrame frame_of(const nrvk::Context::Image& im) {
    nr::ColourFrame f; f.image=im.handle; f.format=im.format; f.width=im.w; f.height=im.h;
    f.before=f.after=VK_IMAGE_LAYOUT_GENERAL;
    f.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return f;
}
vknr::Result fail(Session* s,const char* message) {
    lastError=message; if(s){s->error=message;s->poisoned=true;} return vknr::Result::Failed;
}
}
#define API extern "C" __declspec(dllexport)
API vknr::Result __cdecl vknrQuery(ID3D12Device* d3d,const wchar_t* root,vknr::Capability* caps,vknr::Diagnostics diagnostics) {
    std::lock_guard<std::mutex> lock(apiMutex);
    if(!caps||caps->size!=sizeof(*caps)||caps->version!=vknr::Version||!root)return vknr::Result::Refused;
    *caps={};
    if(!d3d){strcpy_s(caps->reason,"Vulkan NR needs the rendering device");return vknr::Result::Refused;}
    const auto key=adapter_key(d3d);
    if(const auto found=admissions.find(key);found!=admissions.end()) {
        *caps=found->second;return caps->available?vknr::Result::Ok:vknr::Result::Refused;
    }
    try { auto probe=vknr::make_device(d3d,root,false,*caps,diagnostics); }
    catch(const std::exception& e){caps->available=false;strncpy_s(caps->reason,e.what(),_TRUNCATE);lastError=e.what();}
    admissions.emplace(key,*caps);
    return caps->available?vknr::Result::Ok:vknr::Result::Refused;
}
API vknr::Result __cdecl vknrCreate(const vknr::Create* info,vknr::Handle* handle,vknr::Capability* caps) {
    std::lock_guard<std::mutex> lock(apiMutex);
    if(!handle)return vknr::Result::Refused; *handle=nullptr;
    if(!info||info->size!=sizeof(*info)||info->version!=vknr::Version||!caps||caps->size!=sizeof(*caps)||
        !info->device||!info->weightsPath||!info->shaderRoot||!info->cacheRoot||!info->fence)return vknr::Result::Refused;
    const auto admitted=admissions.find(adapter_key(info->device));
    if(admitted==admissions.end()||!admitted->second.available) {
        lastError="Vulkan NR needs a successful selected capability check before initialization";
        return vknr::Result::Refused;
    }
#ifndef TRP_VKNR_VALIDATION
    if(info->validation){lastError="Validation is available only in debug/test builds";return vknr::Result::Refused;}
#endif
    auto session=std::make_unique<Session>();
    try {
        auto& s=*session; *caps={};
        s.owner=vknr::make_device(info->device,info->shaderRoot,info->validation,*caps,nullptr,&admitted->second);
        std::string why;
        if(!s.ctx.adopt(vknr::instance(s.owner.get()),vknr::physical(s.owner.get()),vknr::device(s.owner.get()),
            vknr::queue(s.owner.get()),vknr::family(s.owner.get()),&why))throw std::runtime_error(why);
        for(unsigned i=0;i<4;++i)s.import_image(i,info->images[i]);
        if(s.images[0].w!=s.images[3].w||s.images[0].h!=s.images[3].h)throw std::runtime_error("Vulkan NR output extent mismatch");
        VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};timeline.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};semaphore.pNext=&timeline;
        NRVK_CHECK(vkCreateSemaphore(s.ctx.device,&semaphore,nullptr,&s.semaphore));
        auto import=reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(s.ctx.device,"vkImportSemaphoreWin32HandleKHR"));
        if(!import)throw std::runtime_error("Missing Win32 timeline import function");
        VkImportSemaphoreWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        imp.semaphore=s.semaphore;imp.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;imp.handle=info->fence;
        NRVK_CHECK(import(s.ctx.device,&imp));
        s.uncertain=true;
        s.ctx.one_shot([&](VkCommandBuffer cmd){s.ownership(cmd,false,true);}); // initialization only
        s.uncertain=false;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex=s.ctx.family;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        NRVK_CHECK(vkCreateCommandPool(s.ctx.device,&pool,nullptr,&s.pool));
        for(auto& slot:s.slots) {
            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};alloc.commandPool=s.pool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=1;
            NRVK_CHECK(vkAllocateCommandBuffers(s.ctx.device,&alloc,&slot.command));
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};NRVK_CHECK(vkCreateFence(s.ctx.device,&fence,nullptr,&slot.fence));
        }
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};query.queryType=VK_QUERY_TYPE_TIMESTAMP;query.queryCount=vknr::Slots*2;
        NRVK_CHECK(vkCreateQueryPool(s.ctx.device,&query,nullptr,&s.queries));
        auto weights=nrtest::load_small_weights(info->weightsPath);
        nr::HostDevice host;host.instance=vknr::instance(s.owner.get());host.physical=s.ctx.physical;host.device=s.ctx.device;host.queue=s.ctx.queue;host.queue_family=s.ctx.family;
        nr::RuntimeConfig config;config.root=(std::filesystem::path(info->shaderRoot)/(caps->path==vknr::Path::RDNA3?"rdna3":"portable")).u8string();
        config.width=s.images[0].w;config.height=s.images[0].h;config.network=1;config.colour_format=s.images[0].format;config.motion_point=true;
        config.cache_file=(std::filesystem::path(info->cacheRoot)/(caps->path==vknr::Path::RDNA3?"rdna3.cache":"portable.cache")).u8string();
        nr::TemporalConfig temporal;temporal.enable=true;temporal.shaders=(std::filesystem::path(config.root)/"dlssnr-amd/shaders/temporal").u8string();
        nr::standalone::weights=&weights.entries;
        s.uncertain=true; // a throwing constructor retains its partially built Impl
        try{s.runtime=std::make_unique<nr::Runtime>(host,config,nr::ControlMaskConfig{},temporal);}
        catch(...){nr::standalone::weights=nullptr;throw;}
        nr::standalone::weights=nullptr;
        s.uncertain=false;
        if(s.runtime->network()!=nr::NetworkModel::Small)throw std::runtime_error("Vulkan NR small model was not selected");
        *handle=session.release();return vknr::Result::Ok;
    } catch(const std::exception& e){
        if(session->uncertain){auto* retained=session.release();*handle=retained;return fail(retained,e.what());}
        return fail(nullptr,e.what());
    }
}
API vknr::Result __cdecl vknrPrepare(vknr::Handle handle,const vknr::Frame* frame) {
    std::lock_guard<std::mutex> lock(apiMutex);auto* s=static_cast<Session*>(handle);
    if(!s||!frame||frame->size!=sizeof(*frame)||frame->slot>=vknr::Slots)return vknr::Result::Refused;
    if(s->poisoned||s->pending)return fail(s,"Vulkan NR session has an abandoned or failed frame");
    if(frame->ready<=s->lastDone||frame->done!=frame->ready+1||!frame->done||
        !std::isfinite(frame->motionScaleX)||!std::isfinite(frame->motionScaleY))return fail(s,"Vulkan NR fence values or motion scale invalid");
    try {
        auto& slot=s->slots[frame->slot];
        if(slot.submitted) {
            const auto result=vkGetFenceStatus(s->ctx.device,slot.fence);
            if(result==VK_NOT_READY)return vknr::Result::Busy;
            NRVK_CHECK(result);
            s->harvest(frame->slot);
            nr::gpu::reset(slot.command);NRVK_CHECK(vkResetFences(s->ctx.device,1,&slot.fence));slot.submitted=false;
        }
        NRVK_CHECK(vkResetCommandBuffer(slot.command,0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        NRVK_CHECK(vkBeginCommandBuffer(slot.command,&begin));
        nr::gpu::Scope scope(nr::gpu::recording(slot.command));
        vkCmdResetQueryPool(slot.command,s->queries,frame->slot*2,2);
        s->ownership(slot.command,true);
        vkCmdWriteTimestamp(slot.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,s->queries,frame->slot*2);
        nr::EngineFrame f;f.colour=frame_of(s->images[0]);f.motion=frame_of(s->images[1]);f.depth=frame_of(s->images[2]);f.target=frame_of(s->images[3]);
        f.motion_scale_x=frame->motionScaleX;f.motion_scale_y=frame->motionScaleY;f.reset=frame->reset;f.depth_inverted=frame->depthInverted;f.feature=1;
        nr::Controls controls;controls.intensity=1;controls.style=frame->style;controls.local_tone=frame->localTone;
        controls.local_structure=frame->localStructure;controls.skin_structure=frame->skin;controls.automatic_mask=frame->autoSkin;
        const auto result=s->runtime->record_engine(slot.command,f,controls);
        if(!result.frame.applied)throw std::runtime_error("Vulkan NR graph refused the frame");
        vkCmdWriteTimestamp(slot.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s->queries,frame->slot*2+1);
        s->ownership(slot.command,false);NRVK_CHECK(vkEndCommandBuffer(slot.command));
        s->prepared=*frame;s->pending=true;return vknr::Result::Ok;
    }catch(const std::exception& e){return fail(s,e.what());}
}
API vknr::Result __cdecl vknrSubmit(vknr::Handle handle) {
    std::lock_guard<std::mutex> lock(apiMutex);auto* s=static_cast<Session*>(handle);
    if(!s||!s->pending||s->poisoned)return vknr::Result::Refused;
    auto& slot=s->slots[s->prepared.slot];
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount=timeline.signalSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&s->prepared.ready;timeline.pSignalSemaphoreValues=&s->prepared.done;
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&timeline;submit.waitSemaphoreCount=submit.signalSemaphoreCount=1;
    submit.pWaitSemaphores=submit.pSignalSemaphores=&s->semaphore;submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&slot.command;
    const auto result=vkQueueSubmit(s->ctx.queue,1,&submit,slot.fence);
    if(result!=VK_SUCCESS){s->uncertain=true;return fail(s,"Vulkan queue submission failed; owner retained and no consumer wait queued");}
    slot.submitted=true;slot.done=s->prepared.done;s->lastDone=slot.done;s->pending=false;
    // Exact per-recording proof, even after a slot's VkFence is reused.
    auto completed=std::make_shared<bool>(false);const auto expected=slot.done;
    nr::gpu::recording(slot.command)->submitted([s,&slot,expected,completed]{
        if(!*completed)*completed=slot.done!=expected||vkGetFenceStatus(s->ctx.device,slot.fence)==VK_SUCCESS;
        return *completed;
    });
    return vknr::Result::Ok;
}
API vknr::Result __cdecl vknrRelease(vknr::Handle handle) {
    std::lock_guard<std::mutex> lock(apiMutex);auto* s=static_cast<Session*>(handle);if(!s)return vknr::Result::Ok;
    try {if(!s->retired())return vknr::Result::Busy;delete s;return vknr::Result::Ok;}
    catch(const std::exception& e){return fail(s,e.what());}
}
API vknr::Result __cdecl vknrPoll(vknr::Handle handle,vknr::Completion* completion) {
    std::lock_guard<std::mutex> lock(apiMutex);auto* s=static_cast<Session*>(handle);
    if(!s||!completion||completion->size!=sizeof(*completion))return vknr::Result::Refused;
    if(s->uncertain)return vknr::Result::Failed;
    try {
        for(unsigned i=0;i<vknr::Slots;++i)if(s->slots[i].submitted){
            const auto r=vkGetFenceStatus(s->ctx.device,s->slots[i].fence);
            if(r==VK_NOT_READY)continue;
            NRVK_CHECK(r);s->harvest(i);
        }
        completion->done=s->completedDone;completion->milliseconds=s->milliseconds;
        completion->samples=s->samples;completion->totalMilliseconds=s->totalMilliseconds;return vknr::Result::Ok;
    }catch(const std::exception& e){s->uncertain=true;return fail(s,e.what());}
}
API vknr::Result __cdecl vknrDrain(vknr::Handle handle) {
    std::lock_guard<std::mutex> lock(apiMutex);auto* s=static_cast<Session*>(handle);
    if(!s)return vknr::Result::Ok;
    if(s->uncertain)return vknr::Result::Failed;
    std::array<VkFence,vknr::Slots> fences{};unsigned count=0;
    for(auto& slot:s->slots)if(slot.submitted)fences[count++]=slot.fence;
    if(count&&vkWaitForFences(s->ctx.device,count,fences.data(),VK_TRUE,30'000'000'000ull)!=VK_SUCCESS){
        s->uncertain=true;return fail(s,"Vulkan teardown drain unconfirmed; owner retained");
    }
    return vknr::Result::Ok;
}
API const char* __cdecl vknrLastError(vknr::Handle handle) {auto* s=static_cast<Session*>(handle);return s?s->error.c_str():lastError.c_str();}
API double __cdecl vknrTiming(vknr::Handle handle) {auto* s=static_cast<Session*>(handle);return s?s->milliseconds:-1;}
