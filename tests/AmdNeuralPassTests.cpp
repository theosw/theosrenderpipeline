#include "FrameGen/AmdNeuralPass.h"
#include "FrameGen/NeuralRuntimeSession.h"
#include <chrono>
#include <thread>
#include <dxgi1_4.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace TheosRenderPipeline::SourceDLSSG;
using Microsoft::WRL::ComPtr;
namespace {
    void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    void HR(HRESULT value) { Require(SUCCEEDED(value), "D3D12 operation failed"); }
    struct Mock {
        unsigned creates{}, processes{}, submissions{}, destroys{};
        bool failCreate{}, failState{}, failProcess{};
        int expectedWait{}, expectedTracking{};
        dlss5nr_frame last{};
    } mock;
    void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; list->ResourceBarrier(1,&barrier);
    }
    int Create(const dlss5nr_create_info* info, dlss5nr_context** out) {
        Require(info->device && info->struct_size == sizeof(trp_dlss5nr_create_info), "size-checked adapter selection creation contract");
        ++mock.creates;
        if (mock.failCreate) return -3;
        *out = reinterpret_cast<dlss5nr_context*>(&mock); return 0;
    }
    void Destroy(dlss5nr_context*) { ++mock.destroys; }
    HANDLE releaseStuck{};
    std::atomic_uint stuckDestroys{};
    // A runtime whose worker never returns from a HIP call.
    void StuckDestroy(dlss5nr_context*) { WaitForSingleObject(releaseStuck, INFINITE); ++stuckDestroys; }
    void Defaults(dlss5nr_settings* s) { *s = {}; s->struct_size = sizeof(*s); }
    void Settings(dlss5nr_context*, const dlss5nr_settings* s) {
        Require(s->wait_for_network == mock.expectedWait && s->follow_motion == 1 && s->network_history == 0 && s->show_tracking == mock.expectedTracking,
                "live wait-mode contract with unchanged motion/history");
    }
    int Process(dlss5nr_context*, const dlss5nr_frame* f) {
        ++mock.processes; mock.last = *f;
        Require(f->input_state == D3D12_RESOURCE_STATE_COMMON && f->output_state == D3D12_RESOURCE_STATE_COMMON &&
            f->motion_state == D3D12_RESOURCE_STATE_COMMON && f->depth_state == D3D12_RESOURCE_STATE_COMMON,
            "host states contract");
        Require(f->input != f->output && f->width == 8 && f->height == 4 &&
            f->motion_width == 16 && f->motion_height == 8 && f->depth_width == 16 && f->depth_height == 8 &&
            f->motion_scale_x == -16 && f->motion_scale_y == 8 && f->depth_inverted == 1 &&
            f->pre_exposure == 1 && f->hdr == 0, "extent/motion/depth/preexposure contract");
        Barrier(f->command_list,f->input,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(f->command_list,f->output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        if (mock.failProcess) return -19; // Unknown partly recorded work: host must retain/cancel, not guess a copy.
        f->command_list->CopyResource(f->output,f->input);
        Barrier(f->command_list,f->output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        Barrier(f->command_list,f->input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        return 0;
    }
    void Present(dlss5nr_context*, ID3D12CommandQueue* queue) { Require(queue != nullptr,"queue contract"); ++mock.submissions; }
    void Status(dlss5nr_context*, dlss5nr_status* s) {
        s->state = mock.failState ? DLSS5NR_STATE_FAILED : DLSS5NR_STATE_RUNNING;
        s->results = mock.submissions; strcpy_s(s->message, "mock C API; no HIP or inference");
    }
    AmdNeuralPass::API API() { return {Create,Destroy,Defaults,Settings,Process,Present,Status}; }
    struct GPU {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence,gate;
        UINT64 value{};
        HANDLE event = CreateEventW(nullptr,FALSE,FALSE,nullptr);
        std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
        std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
        GPU() {
            ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
            HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
            HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
            D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            HR(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
            HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
            HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
            Require(event != nullptr,"event");
        }
        ~GPU() { CloseHandle(event); }
        ID3D12GraphicsCommandList* Begin() {
            ComPtr<ID3D12CommandAllocator> a; ComPtr<ID3D12GraphicsCommandList> l;
            HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));
            HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)));
            allocators.push_back(a); lists.push_back(l); return l.Get();
        }
        void Submit(ID3D12GraphicsCommandList* list) {
            HR(list->Close()); ID3D12CommandList* batch[]{list}; queue->ExecuteCommandLists(1,batch);
            HR(queue->Signal(fence.Get(),++value));
        }
        void Wait() {
            HR(fence->SetEventOnCompletion(value,event)); Require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU completion");
        }
        ComPtr<ID3D12Resource> Texture(UINT w,UINT h,DXGI_FORMAT format) {
            D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w;d.Height=h;
            d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;
            D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> r;
            HR(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));
            return r;
        }
        ComPtr<ID3D12Resource> Buffer(UINT64 size,D3D12_HEAP_TYPE type) {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;
            d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES p{};p.Type=type;ComPtr<ID3D12Resource> r;
            HR(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?
                D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));
            return r;
        }
        ComPtr<ID3D12Resource> Upload(ID3D12GraphicsCommandList* list,ID3D12Resource* color,unsigned char fill) {
            auto up=Buffer(256*4,D3D12_HEAP_TYPE_UPLOAD);void* mapped{};HR(up->Map(0,nullptr,&mapped));
            memset(mapped,fill,256*4);up->Unmap(0,nullptr);
            D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=color;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            b.pResource=up.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,8,4,1,256};
            Barrier(list,color,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyTextureRegion(&a,0,0,0,&b,nullptr);Barrier(list,color,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
            return up;
        }
        ComPtr<ID3D12Resource> Read(ID3D12GraphicsCommandList* list,ID3D12Resource* output) {
            auto r=Buffer(256*4,D3D12_HEAP_TYPE_READBACK);D3D12_TEXTURE_COPY_LOCATION a{},b{};
            a.pResource=r.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,8,4,1,256};
            b.pResource=output;b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Barrier(list,output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyTextureRegion(&a,0,0,0,&b,nullptr);Barrier(list,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
            return r;
        }
        void Pixels(ID3D12Resource* r,unsigned char fill) {
            unsigned char* p{};HR(r->Map(0,nullptr,reinterpret_cast<void**>(&p)));
            for(unsigned y=0;y<4;++y) for(unsigned x=0;x<8*4;++x) Require(p[y*256+x]==fill,"queued/fallback pixel mismatch including alpha");
            r->Unmap(0,nullptr);
        }
    };
}
int main() {
    try {
        NeuralOptions options;options.enabled=true;options.beforeUpscaling=true;options.passes=1;
        options.reconstruction.inputScale=.5f;
        auto s=AmdNeuralPass::Settings(options); Require(s.resolution_scale==.5f && s.max_age==8 && s.wait_for_network==0,"defaults");
        Require(s.network_preset==0,"new smaller default model mapping");
        auto previous=options; previous.reconstruction.preset=1;
        Require(AmdNeuralPass::Settings(previous).network_preset==1,"previous larger model mapping");
        Require(s.passes==1 && s.keep_local_tone==1 && s.blend_frames==4 && s.wait_inside==0,
            "C13 adopts blending without extra passes or same-frame GPU waiting");
        auto invalid=options;invalid.reconstruction.inputScale=std::numeric_limits<float>::quiet_NaN();
        invalid.tuning.style=7;invalid.tuning.intensity=2;invalid.tuning.skinStructureStrength=-1;
        s=AmdNeuralPass::Settings(invalid);Require(s.resolution_scale==.5f&&s.style==2&&s.intensity==1&&s.skin_structure==-1,"bounded controls");
        GPU gpu;Require(!AmdNeuralPass::IsAMD(gpu.device.Get()),"WARP is not an AMD runtime test");
        auto motion=gpu.Texture(16,8,DXGI_FORMAT_R16G16_FLOAT),depth=gpu.Texture(16,8,DXGI_FORMAT_R32_FLOAT);
        std::vector<ComPtr<ID3D12Resource>> colors,uploads,reads;
        {
            AmdNeuralPass owner(API()); HR(gpu.queue->Wait(gpu.gate.Get(),1));
            for(unsigned i=0;i<3;++i) {
                options.amdWaitForNetwork = i==1; mock.expectedWait = i==1;
                options.amdShowTracking = i==1; mock.expectedTracking = i==1;
                Require(!owner.NeedsRecreation(options,16,8,nullptr),"wait-mode switch must preserve retained resources");
                auto color=gpu.Texture(8,4,DXGI_FORMAT_R8G8B8A8_UNORM); auto* list=gpu.Begin();
                uploads.push_back(gpu.Upload(list,color.Get(),static_cast<unsigned char>(31+i)));
                Require(owner.Record(gpu.device.Get(),list,options,i==0,true,-16,8,motion.Get(),depth.Get(),color.Get()),"record queued host frame");
                Require(mock.last.reset==(i==0),"history reset continuity");
                if (i == 0) Require(!owner.Running(), "loaded worker is inactive before first composed result");
                Require(!owner.Record(gpu.device.Get(),list,options,false,true,-16,8,motion.Get(),depth.Get(),color.Get()),"reject unsubmitted duplicate");
                reads.push_back(gpu.Read(list,owner.Output()));gpu.Submit(list);
                Require(owner.Submitted(gpu.queue.Get()),"notify after submission"); Require(owner.Running(), "composed output activates the host snapshot"); colors.push_back(color);
            }
            Require(mock.creates==1&&mock.processes==3&&mock.submissions==3&&gpu.fence->GetCompletedValue()==0,"three host frames admitted before GPU retirement");
            HR(gpu.gate->Signal(1));gpu.Wait();for(unsigned i=0;i<3;++i) gpu.Pixels(reads[i].Get(),static_cast<unsigned char>(31+i));
            auto resized=gpu.Texture(9,4,DXGI_FORMAT_R8G8B8A8_UNORM);
            Require(owner.NeedsRecreation(options,16,8,resized.Get()),"scene-only resize requires retirement");
            auto changed=options;changed.reconstruction.inputScale=.75f;
            Require(owner.NeedsRecreation(changed,16,8,colors.back().Get()),"network resize requires retirement");
            const auto processes=mock.processes;mock.failState=true;auto* list=gpu.Begin();
            Require(owner.Record(gpu.device.Get(),list,options,false,true,-16,8,motion.Get(),depth.Get(),colors.back().Get()),"model failure original-copy fallback");
            auto read=gpu.Read(list,owner.Output());gpu.Submit(list);Require(owner.Submitted(gpu.queue.Get()),"fallback submitted");gpu.Wait();
            gpu.Pixels(read.Get(),33);Require(mock.processes==processes&&owner.Failed(),"model failure remains local to NR");
        }
        Require(mock.destroys==1,"runtime destroyed once after host retirement");
        mock.failState=false;
        {
            AmdNeuralPass recreated(API());auto* list=gpu.Begin();
            Require(recreated.Record(gpu.device.Get(),list,options,true,true,-16,8,motion.Get(),depth.Get(),colors.back().Get()),"recreate retired owner");
            auto read=gpu.Read(list,recreated.Output());gpu.Submit(list);Require(recreated.Submitted(gpu.queue.Get()),"recreated submission");gpu.Wait();gpu.Pixels(read.Get(),33);
            mock.failProcess=true;list=gpu.Begin();const auto submitted=mock.submissions;
            Require(!recreated.Record(gpu.device.Get(),list,options,false,true,-16,8,motion.Get(),depth.Get(),colors.back().Get()),"reject partial recording failure");
            Require(recreated.Output()!=nullptr&&mock.submissions==submitted,"retain output and do not notify unsubmitted failure");
            HR(list->Close());HR(gpu.allocators.back()->Reset());HR(list->Reset(gpu.allocators.back().Get(),nullptr));
            HR(list->Close()); // Explicitly discard the unsubmitted commands before releasing their resources.
        }
        Require(mock.creates==2&&mock.destroys==2,"recreated context retired exactly once");
        mock={};mock.failCreate=true;
        {
            AmdNeuralPass owner(API());auto* list=gpu.Begin();
            Require(owner.Record(gpu.device.Get(),list,options,true,true,-16,8,motion.Get(),depth.Get(),colors.back().Get()),"initialization failure copy fallback");
            auto read=gpu.Read(list,owner.Output());gpu.Submit(list);gpu.Wait();gpu.Pixels(read.Get(),33);
            Require(owner.Failed()&&mock.processes==0&&mock.submissions==0,"no GPU evaluator use after failed initialization");
        }
        {   // Stuck runtime shutdown: bounded, leaked rather than freed, HIP refused for the session.
            mock={};releaseStuck=CreateEventW(nullptr,TRUE,FALSE,nullptr);Require(releaseStuck!=nullptr,"stuck-worker event");
            AmdNeuralPass::destroyTimeout=std::chrono::milliseconds(200);
            std::string logged;
            const auto started=std::chrono::steady_clock::now();
            {
                auto api=API();api.destroy=StuckDestroy;
                AmdNeuralPass owner(api);owner.log=[&](int level,const char* text){if(level==0)logged=text;};
                auto* list=gpu.Begin();
                Require(owner.Record(gpu.device.Get(),list,options,true,true,-16,8,motion.Get(),depth.Get(),colors.back().Get()),"stuck-shutdown owner records");
                gpu.Submit(list);Require(owner.Submitted(gpu.queue.Get()),"stuck-shutdown submission");gpu.Wait();
            }
            const auto elapsed=std::chrono::steady_clock::now()-started;
            Require(elapsed<std::chrono::seconds(3),"teardown returns although the runtime worker is stuck");
            Require(stuckDestroys==0,"stuck destroy is still running, its context not freed underneath it");
            Require(logged.find("HIP/ZLUDA shutdown did not finish")!=std::string::npos,"stuck shutdown is logged as an error");
            using namespace TheosRenderPipeline::NeuralRendering;
            Require(runtimeSession.Refusal(Backend::HIP)==kHIPStalled,"HIP refused until restart");
            Require(runtimeSession.Refusal(Backend::Native)==nullptr,"native NR is unaffected");
            SetEvent(releaseStuck);
            for(int i=0;i<200&&stuckDestroys==0;++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Require(stuckDestroys==1,"late destroy completes once on its own thread");
            CloseHandle(releaseStuck);
        }
        puts("PASS: CPU controls, C API mapping, three GPU-gated host submissions, pixels/alpha, reset continuity, scene/network resize, model/create failure fallback, retirement, bounded stuck shutdown. No HIP inference.");
        return 0;
    } catch(const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
}
