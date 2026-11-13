#include <PCH.h>
#include "VulkanNRHost.h"
#include "NeuralRuntimeSession.h"
#include "../PluginPaths.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "TRPNeuralShaders.generated.h"
#include <mutex>
#include <map>
#include <chrono>
#include <format>
#include <cassert>
#include <thread>
#include <atomic>

namespace TheosRenderPipeline::NeuralRendering {
namespace {
struct API {
    HMODULE module{};
    vknr::Query query{};vknr::CreateFn create{};vknr::Prepare prepare{};vknr::Submit submit{};
    vknr::Release release{};vknr::LastError error{};vknr::Timing timing{};
    vknr::Poll poll{};vknr::Drain drain{};
    bool Load() {
        static std::mutex loadMutex; std::scoped_lock lock(loadMutex);
        if(module)return query&&create&&prepare&&submit&&release&&error&&timing&&poll&&drain;
        const auto path=VulkanNRHost::Path();
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module)return false;
#define LOAD(field,type,name) field=reinterpret_cast<type>(GetProcAddress(module,name))
        LOAD(query,vknr::Query,"vknrQuery");LOAD(create,vknr::CreateFn,"vknrCreate");LOAD(prepare,vknr::Prepare,"vknrPrepare");
        LOAD(submit,vknr::Submit,"vknrSubmit");LOAD(release,vknr::Release,"vknrRelease");LOAD(error,vknr::LastError,"vknrLastError");LOAD(timing,vknr::Timing,"vknrTiming");
        LOAD(poll,vknr::Poll,"vknrPoll");LOAD(drain,vknr::Drain,"vknrDrain");
#undef LOAD
        return query&&create&&prepare&&submit&&release&&error&&timing&&poll&&drain;
    }
};
API api;std::mutex probeMutex;
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
}
std::filesystem::path VulkanNRHost::Directory(){
#ifdef TRP_NR_VENDOR_BENCHMARK
    wchar_t test[32768]{};const auto n=GetEnvironmentVariableW(L"TRP_VKNR_TEST_ROOT",test,32768);
    if(n&&n<32768)return test;
#endif
    return PluginPaths::Directory()/L"TheosRenderPipeline"/L"VulkanNR";
}
std::filesystem::path VulkanNRHost::Path(){return Directory()/L"TRPVulkanNR.dll";}
std::filesystem::path VulkanNRHost::Weights(){
#ifdef TRP_NR_VENDOR_BENCHMARK
    wchar_t test[32768]{};const auto n=GetEnvironmentVariableW(L"TRP_VKNR_TEST_WEIGHTS",test,32768);
    if(n&&n<32768)return test;
#endif
    return PluginPaths::Directory()/L"TheosRenderPipeline"/L"NVIDIA"/L"nvngx_dlssnr_310.9.16.dll";
}
vknr::Capability VulkanNRHost::Probe(ID3D12Device* device, bool wait, bool request) {
    if (const auto* reason = runtimeSession.Refusal(Backend::Vulkan)) { vknr::Capability result; strcpy_s(result.reason,reason); return result; }
    struct Job { std::atomic_bool done{}; vknr::Capability result; };
    static std::map<std::uint64_t, std::shared_ptr<Job>> cache;
    std::shared_ptr<Job> job;
    {
        std::scoped_lock lock(probeMutex);
        vknr::Capability result;
        if (!device) { strcpy_s(result.reason, request ? "Vulkan NR needs the rendering device" : kVulkanNotChecked); return result; }
        const auto luid = device->GetAdapterLuid();
        const auto key = (std::uint64_t(std::uint32_t(luid.HighPart)) << 32) | luid.LowPart;
        auto found = cache.find(key);
        if (found == cache.end() && !request) { strcpy_s(result.reason,kVulkanNotChecked); return result; }
        auto& entry = cache[key];
        if (!entry) {
            entry = std::make_shared<Job>();
            Microsoft::WRL::ComPtr<ID3D12Device> held = device;
            logger::info("[Vulkan NR] probe requested by explicit Vulkan selection; adapter LUID={:08X}:{:08X}",
                std::uint32_t(luid.HighPart),luid.LowPart);
            try { std::thread([pending = entry, held] {
                try {
                    std::error_code ec;
                    if (const auto* reason=runtimeSession.Refusal(Backend::Vulkan)) strcpy_s(pending->result.reason,reason);
                    else if (!std::filesystem::is_regular_file(Weights(),ec))
                        strcpy_s(pending->result.reason,"Vulkan NR needs Standard's NVIDIA/nvngx_dlssnr_310.9.16.dll. Install Standard below Universal, or supply that matching runtime.");
                    else if (!std::filesystem::is_regular_file(Path(),ec))
                        strcpy_s(pending->result.reason,"Vulkan NR engine is missing. Reinstall Universal.");
                    else if (!NeuralRenderingRuntimeIdentity::VerifyExpected(Weights(),266424944,"2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F").matched)
                        strcpy_s(pending->result.reason,"Vulkan NR needs the original NVIDIA 310.9.16 model from Standard; the installed file does not match.");
                    // Claim immediately before loading: HIP may have won while prerequisites were checked.
                    // This claim persists because the engine DLL and driver modules stay loaded.
                    else if (!runtimeSession.BeginVulkanProbe()) strcpy_s(pending->result.reason,kRuntimeRestart);
                    else if (!api.Load()) strcpy_s(pending->result.reason,"Vulkan NR engine ABI is incomplete. Reinstall Universal.");
                    else {
                        const auto queried=api.query(held.Get(), Directory().c_str(), &pending->result, [](const char* json) {
                            logger::info("[Vulkan NR] probe device capabilities: {}",json);
                        });
                        if (queried!=vknr::Result::Ok) pending->result.available=false;
                        if (!pending->result.available && !pending->result.reason[0])
                            strcpy_s(pending->result.reason,"Vulkan NR engine ABI/query refused this host. Reinstall the matching Universal package.");
                    }
                } catch (...) { strcpy_s(pending->result.reason,"Vulkan NR capability query failed"); }
                const auto& c=pending->result;
                logger::info("[Vulkan NR] probe complete available={} path={} reason={} pathReason={} device={} revision={}; cached for this session",
                    c.available,c.available ? (c.path==vknr::Path::RDNA3 ? "rdna3" : "portable") : "refused",
                    c.reason,c.pathReason,c.device,c.revision);
                pending->done.store(true,std::memory_order_release);
            }).detach(); } catch (...) {
                strcpy_s(entry->result.reason,"Vulkan NR capability worker could not start");
                logger::warn("[Vulkan NR] probe refused: {}; cached for this session",entry->result.reason);
                entry->done.store(true,std::memory_order_release);
            }
        }
        job = entry;
    }
    // Only the selected backend's background preparation worker/manual smoke waits.
    if (wait) while (!job->done.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (job->done.load(std::memory_order_acquire)) return job->result;
    vknr::Capability pending;
    strcpy_s(pending.reason,kVulkanChecking);
    return pending;
}
bool VulkanNRHost::Shaders(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,3}};
    D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,4};
    D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob,errors;
    if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors))||
        FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_))))return false;
    using namespace SourceDLSSG::CompiledNeuralShaders;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root_.Get();pso.CS={VulkanPack,sizeof(VulkanPack)};
    if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pack_))))return false;
    pso.CS={VulkanUnpack,sizeof(VulkanUnpack)};
    if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&unpack_))))return false;
    for(auto& heap:heaps_){D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,12,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        if(FAILED(device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&heap))))return false;}
    return true;
}
bool VulkanNRHost::Initialize(const FeatureSession::CreateInfo& info) {
    capability_=Probe(info.device, true, true);
    if(!capability_.available){status_=capability_.reason;return false;}
    if (const auto* reason = runtimeSession.Claim(Backend::Vulkan)) { status_=reason; return false; }
    device_=info.device;width_=info.displayWidth;height_=info.displayHeight;guideWidth_=info.renderWidth;guideHeight_=info.renderHeight;
    if(!Shaders(info.device)){status_="Vulkan NR conversion shaders could not initialize";return false;}
    // Copy timestamps are in the host queue's domain; inference stays in Vulkan's.
    D3D12_QUERY_HEAP_DESC q{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,vknr::Slots*4,0};
    D3D12_RESOURCE_DESC readback{};readback.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    readback.Width=sizeof(std::uint64_t)*vknr::Slots*4;readback.Height=1;readback.DepthOrArraySize=readback.MipLevels=1;
    readback.SampleDesc.Count=1;readback.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES readHeap{};readHeap.Type=D3D12_HEAP_TYPE_READBACK;
    if(SUCCEEDED(device_->CreateQueryHeap(&q,IID_PPV_ARGS(&queries_)))&&
        SUCCEEDED(device_->CreateCommittedResource(&readHeap,D3D12_HEAP_FLAG_NONE,&readback,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&queryReadback_)))){
        D3D12_RANGE range{0,SIZE_T(readback.Width)};
        if(FAILED(queryReadback_->Map(0,&range,reinterpret_cast<void**>(&mappedQueries_))))queries_.Reset();
    }else queries_.Reset();
    vknr::Create create;create.device=info.device;
    const auto weights=Weights(),directory=Directory();
    wchar_t local[32768]{};const auto length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    if(!length||length>=32768){status_="Vulkan NR cache location is unavailable";return false;}
    const auto cache=std::filesystem::path(local)/L"TheosRenderPipeline"/L"PipelineCache"/L"VulkanNR";
    std::error_code ec;std::filesystem::create_directories(cache,ec);
    if(ec){status_="Vulkan NR cache directory could not be created";return false;}
    create.weightsPath=weights.c_str();create.shaderRoot=directory.c_str();create.cacheRoot=cache.c_str();
    struct Handles {std::array<HANDLE,5> values{};~Handles(){for(auto h:values)if(h)CloseHandle(h);}} handles;
    const DXGI_FORMAT formats[]{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
    for(unsigned i=0;i<4;++i) {
        const bool guide=i==1||i==2;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=guide?guideWidth_:width_;desc.Height=guide?guideHeight_:height_;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=formats[i];desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&images_[i])))||
            FAILED(device_->CreateSharedHandle(images_[i].Get(),nullptr,GENERIC_ALL,nullptr,&handles.values[i]))){status_="Vulkan NR shared texture creation refused";return false;}
        create.images[i]={handles.values[i],UINT(desc.Width),desc.Height,desc.Format};
    }
    if(FAILED(device_->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence_)))||
        FAILED(device_->CreateSharedHandle(fence_.Get(),nullptr,GENERIC_ALL,nullptr,&handles.values[4]))){status_="Vulkan NR shared fence creation refused";return false;}
    create.fence=handles.values[4];const auto started=std::chrono::steady_clock::now();
    // A GPU device loss or hang inside Vulkan would otherwise hold the game's queue on frame.done forever.
    // 10 s without any progress on submitted NR work: far beyond Windows' 2 s GPU
    // timeout, so ordinary hitches, loading and slow frames never reach it.
    watchdog_.Start(fence_.Get(),std::chrono::milliseconds(10000));
#ifdef TRP_NR_VENDOR_BENCHMARK
    create.validation=GetEnvironmentVariableW(L"TRP_VKNR_TEST_VALIDATION",nullptr,0)>0;
#endif
    if(api.create(&create,&handle_,&capability_)!=vknr::Result::Ok){unsafe_=handle_!=nullptr;status_=api.error(handle_);return false;}
    status_=std::format("Vulkan: {} ({})",capability_.path==vknr::Path::RDNA3?"rdna3":"portable",capability_.revision);
    logger::info("[Vulkan NR] backend=Vulkan revision={} path={} device={} model={}x{} guides={}x{} initialization={:.1f} ms; {}",
        capability_.revision,capability_.path==vknr::Path::RDNA3?"rdna3":"portable",capability_.device,width_,height_,guideWidth_,guideHeight_,
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),capability_.pathReason);
    return true;
}
void VulkanNRHost::Dispatch(ID3D12GraphicsCommandList* list,std::size_t slot,const FeatureSession::EvaluationInput& input,bool pack) {
    const auto query=UINT(slot*4+(pack?0:2));
    if(queries_&&queryFrequency_)list->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query);
    const auto stride=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto* heap=heaps_[slot].Get();auto cpu=heap->GetCPUDescriptorHandleForHeapStart();auto gpu=heap->GetGPUDescriptorHandleForHeapStart();
    if(!pack){cpu.ptr+=6*stride;gpu.ptr+=6*stride;}
    ID3D12Resource* sources[]{pack?input.color:images_[3].Get(),input.motionVectors,input.depth};
    ID3D12Resource* dests[]{pack?images_[0].Get():input.output,images_[1].Get(),images_[2].Get()};
    const unsigned count=pack?3:1;
    for(unsigned i=0;i<3;++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=sources[i]->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device_->CreateShaderResourceView(sources[i],&srv,{cpu.ptr+i*stride});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=dests[i]->GetDesc().Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(dests[i],nullptr,&uav,{cpu.ptr+(i+3)*stride});
    }
    for(unsigned i=0;i<count;++i){transition(list,sources[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(list,dests[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
    list->SetDescriptorHeaps(1,&heap);list->SetComputeRootSignature(root_.Get());list->SetPipelineState(pack?pack_.Get():unpack_.Get());
    list->SetComputeRootDescriptorTable(0,gpu);const UINT sizes[]{width_,height_,guideWidth_,guideHeight_};list->SetComputeRoot32BitConstants(1,4,sizes,0);
    list->Dispatch(((pack?(std::max)(width_,guideWidth_):width_)+7)/8,((pack?(std::max)(height_,guideHeight_):height_)+7)/8,1);
    for(unsigned i=0;i<count;++i){transition(list,sources[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        transition(list,dests[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);}
    if(queries_&&queryFrequency_){
        list->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query+1);
        list->ResolveQueryData(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query,2,queryReadback_.Get(),query*sizeof(std::uint64_t));
        queryPending_[slot][pack?0:1]=true;
    }
}
bool VulkanNRHost::WatchdogFired() {
    if(!watchdog_.Fired())return false;
    if(!unsafe_)logger::error("[Vulkan NR] backend=Vulkan stalled: value {} not signalled within {} ms; "
        "D3D12 queue released, Vulkan NR off for this session",watchdog_.FiredValue(),watchdog_.Timeout().count());
    runtimeSession.Fail(Backend::Vulkan,kVulkanStalled);
    unsafe_=true;status_=kVulkanStalled;return true;
}
bool VulkanNRHost::BeginFrame(std::size_t slot,ID3D12CommandQueue* queue,bool& ready) {
    ready=false;if(!handle_||!queue||slot>=vknr::Slots||WatchdogFired())return false;
    ++measurements_.frames;
    if(!queryFrequency_)queue->GetTimestampFrequency(&queryFrequency_);
    for(unsigned kind=0;kind<2;++kind)if(queryPending_[slot][kind]){
        const auto i=slot*4+kind*2;
        if(mappedQueries_&&queryFrequency_&&mappedQueries_[i+1]>=mappedQueries_[i])
            measurements_.copyTotalMs+=double(mappedQueries_[i+1]-mappedQueries_[i])*1000.0/queryFrequency_;
        queryPending_[slot][kind]=false;
    }
    vknr::Completion completion;
    if(api.poll(handle_,&completion)!=vknr::Result::Ok){
        // A failed poll (for example Vulkan device lost) will never signal: release now.
        const bool released=watchdog_.ReleaseNow();
        unsafe_=true;status_=api.error(handle_);
        if(released){runtimeSession.Fail(Backend::Vulkan,kVulkanStalled);
            logger::error("[Vulkan NR] backend=Vulkan engine failure: {}; held D3D12 queue released, Vulkan NR off for this session",status_);}
        return false;}
    measurements_.completed=completion.samples;measurements_.networkTotalMs=completion.totalMilliseconds;
    ready=backgroundReady_=backgroundPending_&&completion.done>=value_;
    return true;
}
bool VulkanNRHost::Consume(const FeatureSession::EvaluationInput& input,ID3D12CommandQueue* queue,std::size_t slot,ID3D12GraphicsCommandList* list) {
    if(!backgroundReady_||!queue||!list||WatchdogFired())return false;
    // Completion has already been proved by VkFence. This wait establishes API
    // ownership/visibility, but cannot hold the host queue behind unfinished NR.
    if(FAILED(queue->Wait(fence_.Get(),value_))){unsafe_=true;status_="Vulkan completed-result wait failed";return false;}
    Dispatch(list,slot,input,false);backgroundPending_=backgroundReady_=false;return true;
}
bool VulkanNRHost::DropCompleted(ID3D12CommandQueue* queue) {
    if(!backgroundReady_||!queue)return false;
    // Reacquire shared-image ownership even when the stale pixels are discarded.
    if(FAILED(queue->Wait(fence_.Get(),value_))){unsafe_=true;status_="Vulkan stale-result ownership wait failed";return false;}
    backgroundPending_=backgroundReady_=false;return true;
}
bool VulkanNRHost::Evaluate(const FeatureSession::EvaluationInput& input,SourceDLSSG::Interop& interop,ID3D12CommandQueue* queue,
    std::size_t slot,ID3D12GraphicsCommandList*& list,bool background,bool frameOpened) {
    if(!handle_||!queue||slot>=vknr::Slots){status_="Vulkan NR submission context missing";return false;}
    if(WatchdogFired())return false;
    if(backgroundPending_){status_="Vulkan job is still reserved";return false;}
    if(!background&&!frameOpened){bool ready{};if(!BeginFrame(slot,queue,ready))return false;}
    const auto color=input.color->GetDesc(),out=input.output->GetDesc(),mv=input.motionVectors->GetDesc(),depth=input.depth->GetDesc();
    if(color.Width!=width_||color.Height!=height_||out.Width!=width_||out.Height!=height_||color.Format!=out.Format||
        (color.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&color.Format!=DXGI_FORMAT_R8G8B8A8_UNORM)||
        mv.Width!=guideWidth_||mv.Height!=guideHeight_||depth.Width!=guideWidth_||depth.Height!=guideHeight_){status_="Vulkan NR input extent/format refused before submission";return false;}
    vknr::Frame frame;frame.slot=UINT(slot);frame.ready=value_+1;frame.done=value_+2;
    // The TRP evaluator expresses model-pixel displacement; the Vulkan engine consumes UV displacement.
    frame.motionScaleX=input.motionVectorScaleX/width_;frame.motionScaleY=input.motionVectorScaleY/height_;
    frame.localTone=input.tuning.localToneStrength;frame.localStructure=input.tuning.localStructureStrength;
    frame.skin=input.tuning.skinStructureStrength;frame.autoSkin=input.tuning.useAutoSkinMask;frame.style=input.tuning.style;
    frame.reset=input.reset;frame.depthInverted=input.depthInverted;
    const auto prepared=api.prepare(handle_,&frame);
    if(prepared!=vknr::Result::Ok){status_=prepared==vknr::Result::Busy?"Vulkan NR slot has not retired":api.error(handle_);return false;}
    Dispatch(list,slot,input,true); // all touched D3D12 resources return to COMMON
    if(FAILED(interop.Split(SourceDLSSG::Work::Upscaling,&list))){unsafe_=true;status_="Vulkan NR producer submission failed";return false;}
    if(FAILED(queue->Signal(fence_.Get(),frame.ready))){unsafe_=true;status_="Vulkan NR ready signal failed";return false;}
    if(api.submit(handle_)!=vknr::Result::Ok){status_=api.error(handle_);return false;}
    value_=frame.done;
    if(background){backgroundPending_=true;++frames_;return true;}
    // Never queue a wait for an unsubmitted value. Track the wait independently
    // of the consumer list so recovery can discard that list and still retire NR.
    if(FAILED(queue->Wait(fence_.Get(),frame.done))){unsafe_=true;status_="Vulkan NR consumer wait could not be queued";return false;}
    watchdog_.Arm(frame.done); // from here the queue is held until Vulkan, or the watchdog, signals frame.done
    if(FAILED(interop.TrackExternal(SourceDLSSG::Work::Upscaling))){
        unsafe_=true;status_="Vulkan NR consumer wait retirement unconfirmed";return false;}
    Dispatch(list,slot,input,false);
    if((++frames_%120)==0)logger::info("[Vulkan NR] path={} completed network={:.3f} ms; host handoff/copies included in NR per frame; frames={}",
        capability_.path==vknr::Path::RDNA3?"rdna3":"portable",api.timing(handle_),frames_);
    return true;
}
bool VulkanNRHost::ReleaseAfterRetirement() {
    if(unsafe_||WatchdogFired())return false;
    if(handle_&&api.drain(handle_)!=vknr::Result::Ok){unsafe_=true;status_="Vulkan shutdown drain unconfirmed; complete owner retained";return false;}
    if(handle_&&api.release(handle_)!=vknr::Result::Ok){status_="Vulkan NR retirement unconfirmed; complete owner retained";return false;}
    handle_=nullptr;watchdog_.Stop();return true;
}
VulkanNRHost::~VulkanNRHost(){
    // The FeatureSession deleter retains this entire owner on a refusal.
    assert(!handle_);
    if(mappedQueries_)queryReadback_->Unmap(0,nullptr);
}
}
