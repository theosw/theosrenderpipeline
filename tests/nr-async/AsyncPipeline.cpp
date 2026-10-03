#include "AsyncPipeline.h"
#include "AsyncShader.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace TRPExperiment {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr) { if(FAILED(hr))throw std::runtime_error("D3D12 async prototype operation failed"); }
bool Complete(ID3D12Fence* fence,std::uint64_t value) {
    if(!fence)return value==0;
    const auto completed=fence->GetCompletedValue();
    return completed!=UINT64_MAX && completed>=value;
}
bool Wait(ID3D12Fence* fence,std::uint64_t value,HANDLE event) {
    if(Complete(fence,value))return true;
    return SUCCEEDED(fence->SetEventOnCompletion(value,event)) &&
        WaitForSingleObject(event,10000)==WAIT_OBJECT_0 && Complete(fence,value);
}
void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,
    D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
}
void Copy(ID3D12GraphicsCommandList* list,ID3D12Resource* source,ID3D12Resource* target) {
    Barrier(list,source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list,target,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(target,source);
    Barrier(list,target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    Barrier(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
}
}

struct AsyncPipeline::State {
    enum Phase { Free, Pending, Evaluating, Ready, Displayed };
    struct Slot {
        Phase phase{Free};
        std::uint64_t frame{}, generation{};
        ComPtr<ID3D12Resource> color,motion,depth,output;
        std::array<ComPtr<ID3D12Resource>,2> flow;
        unsigned flowIndex{};
        ComPtr<ID3D12Fence> captureFence;
        std::uint64_t captureValue{};
    };
    struct Constants {
        unsigned width,height,initialize,useResult;
        float scaleX,scaleY,depthTolerance,colorTolerance,maxRatio;
        unsigned age,maxAge,padding{};
    };
    Config config;
    Evaluator evaluate;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> workerQueue;
    ComPtr<ID3D12CommandAllocator> workerAllocator;
    ComPtr<ID3D12GraphicsCommandList> workerList;
    ComPtr<ID3D12Fence> workerFence,hostFence;
    std::uint64_t workerSerial{},hostValue{};
    bool workerOutstanding{},recording{},stopping{},stopped{},enabledLast{};
    HANDLE event{};
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::array<Slot,3> slots;
    int capture{-1},display{-1};
    Snapshot stats;
    ComPtr<ID3D12Resource> previousDepth,output;
    std::array<ComPtr<ID3D12Resource>,3> hostInputs;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,2> pipelines;
    // One host submission outstanding. Each dispatch needs distinct descriptors.
    std::array<ComPtr<ID3D12DescriptorHeap>,5> heaps;
    unsigned heapIndex{};

    ~State(){if(event)CloseHandle(event);}
    void Fail(const std::string& error) {std::lock_guard lock(mutex);stats.failed=true;stats.error=error;}
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=config.width;desc.Height=config.height;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> resource;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resource)));
        stats.allocationBytes+=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        return resource;
    }
    void Dispatch(ID3D12GraphicsCommandList* list,unsigned kernel,Constants constants,
        std::array<ID3D12Resource*,6> inputs,ID3D12Resource* target) {
        if(heapIndex>=heaps.size())throw std::runtime_error("too many frame dispatches");
        auto* heap=heaps[heapIndex++].Get();auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
        const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        std::vector<ID3D12Resource*> unique;
        for(auto* r:inputs) {
            if(r==target)throw std::runtime_error("async resource alias");
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=r?r->GetDesc().Format:DXGI_FORMAT_R32G32B32A32_FLOAT;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            device->CreateShaderResourceView(r,&srv,cpu);cpu.ptr+=stride;
            if(r && std::find(unique.begin(),unique.end(),r)==unique.end()) {
                unique.push_back(r);Barrier(list,r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=target->GetDesc().Format;
        uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(target,nullptr,&uav,cpu);
        Barrier(list,target,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetDescriptorHeaps(1,&heap);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipelines[kernel].Get());
        list->SetComputeRoot32BitConstants(0,sizeof(constants)/4,&constants,0);
        list->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((config.width+7)/8,(config.height+7)/8,1);
        Barrier(list,target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        for(auto* r:unique)Barrier(list,r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
    void Run() {
        for(;;) {
            int index=-1;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,[&]{
                    return stopping || stats.failed || std::any_of(slots.begin(),slots.end(),[](const auto& s){return s.phase==Pending && s.captureValue;});
                });
                if(stopping || stats.failed)break;
                for(unsigned i=0;i<slots.size();++i)if(slots[i].phase==Pending && slots[i].captureValue &&
                    (index<0 || slots[i].frame>slots[index].frame))index=int(i);
                slots[index].phase=Evaluating;
            }
            auto& slot=slots[index];
            try {
                // CPU waits occur only here, never in Record/Submitted.
                if(!Wait(slot.captureFence.Get(),slot.captureValue,event))throw std::runtime_error("capture completion unconfirmed");
                const auto begin=std::chrono::steady_clock::now();
                Check(workerAllocator->Reset());Check(workerList->Reset(workerAllocator.Get(),nullptr));
                Capture input{slot.color.Get(),slot.motion.Get(),slot.depth.Get(),slot.output.Get(),slot.frame,slot.generation};
                if(!evaluate(workerList.Get(),input))throw std::runtime_error("NR recording failed; no speculative submission");
                Check(workerList->Close());
                ID3D12CommandList* lists[]{workerList.Get()};workerOutstanding=true;
                workerQueue->ExecuteCommandLists(1,lists);
                // A failed Signal leaves work outstanding; Stop retains State.
                Check(workerQueue->Signal(workerFence.Get(),++workerSerial));
                if(!Wait(workerFence.Get(),workerSerial,event))throw std::runtime_error("NR completion unconfirmed");
                workerOutstanding=false;
                std::lock_guard lock(mutex);
                stats.evaluationMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
                ++stats.evaluations;
                slot.phase=Ready; // Includes fence-proven visibility to host.
            } catch(const std::exception& error) {Fail(error.what());break;}
            catch(...) {Fail("NR evaluator exception");break;}
        }
    }
};

AsyncPipeline::AsyncPipeline()=default;
AsyncPipeline::~AsyncPipeline() {
    if(state_ && !Stop()) {
        std::fputs("AsyncPipeline: retaining unretired resources and evaluator for process lifetime\n",stderr);
        (void)state_.release();
    }
}
HRESULT AsyncPipeline::Initialize(ID3D12Device* device,const Config& config,Evaluator evaluate) {
    if(state_ || !device || !evaluate || !config.width || !config.height || config.width>8192 || config.height>8192 ||
        config.maxAge==0 || config.maxAge>120 || !std::isfinite(config.depthTolerance) || config.depthTolerance<0 ||
        !std::isfinite(config.colorTolerance) || config.colorTolerance<0 ||
        !std::isfinite(config.maxRatio) || config.maxRatio<1 || config.maxRatio>8 ||
        !std::isfinite(config.motionScaleX) || !std::isfinite(config.motionScaleY) || !config.motionScaleX || !config.motionScaleY ||
        (config.format!=DXGI_FORMAT_R16G16B16A16_FLOAT && config.format!=DXGI_FORMAT_R32G32B32A32_FLOAT))return E_INVALIDARG;
    // Fixed memory policy for this prototype. Check the adapter's allocation
    // sizes before allocating any history; live budget admission is a separate
    // requirement for a future game integration.
    std::uint64_t required=0;
    const std::array<std::pair<DXGI_FORMAT,unsigned>,4> allocations{{
        {config.format,7},{DXGI_FORMAT_R32_FLOAT,4},{DXGI_FORMAT_R32G32_FLOAT,3},{DXGI_FORMAT_R32G32B32A32_FLOAT,6}}};
    for(auto [format,count]:allocations) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=config.width;desc.Height=config.height;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto bytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        if(bytes==UINT64_MAX || bytes>config.maxAllocationBytes/count || required>config.maxAllocationBytes-bytes*count)return E_OUTOFMEMORY;
        required+=bytes*count;
    }
    auto state=std::make_unique<State>();state->config=config;state->device=device;state->evaluate=std::move(evaluate);
    try {
        state->output=state->Texture(config.format);state->previousDepth=state->Texture(DXGI_FORMAT_R32_FLOAT);
        for(auto& slot:state->slots) {
            slot.color=state->Texture(config.format);slot.output=state->Texture(config.format);
            slot.motion=state->Texture(DXGI_FORMAT_R32G32_FLOAT);slot.depth=state->Texture(DXGI_FORMAT_R32_FLOAT);
            for(auto& flow:slot.flow)flow=state->Texture(DXGI_FORMAT_R32G32B32A32_FLOAT);
        }
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&state->workerQueue)));
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&state->workerAllocator)));
        Check(device->CreateCommandList(0,q.Type,state->workerAllocator.Get(),nullptr,IID_PPV_ARGS(&state->workerList)));
        Check(state->workerList->Close());Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&state->workerFence)));
        state->event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!state->event)throw std::runtime_error("worker event");
        D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,6}};
        D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants={0,0,sizeof(State::Constants)/4};params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable={2,ranges};D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;
        Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
        Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&state->root)));
        const char* names[]{"Track","Compose"};
        for(unsigned i=0;i<2;++i) {
            auto hr=D3DCompile(kAsyncShader,sizeof(kAsyncShader)-1,"TRP-async-prototype",nullptr,nullptr,names[i],"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
            if(FAILED(hr) && error)std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));Check(hr);
            D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=state->root.Get();p.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            Check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&state->pipelines[i])));
        }
        for(auto& heap:state->heaps) {
            D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,7,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            Check(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&heap)));
        }
    }catch(const std::exception&){return E_FAIL;}
    state_=std::move(state);state_->worker=std::thread([s=state_.get()]{s->Run();});return S_OK;
}
HRESULT AsyncPipeline::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* color,
    ID3D12Resource* motion,ID3D12Resource* depth,bool reset,bool enabled) {
    if(!state_ || !list || list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
    auto& s=*state_;std::lock_guard lock(s.mutex);
    if(s.stopping || s.stats.failed || s.recording)return E_UNEXPECTED;
    auto matches=[&](ID3D12Resource* resource,DXGI_FORMAT format) {
        if(!resource)return false;const auto d=resource->GetDesc();ComPtr<ID3D12Device> owner;
        if(FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=s.device.Get())return false;
        return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width==s.config.width && d.Height==s.config.height &&
            d.Format==format && d.DepthOrArraySize==1 && d.MipLevels==1 && d.SampleDesc.Count==1;
    };
    if(!matches(color,s.config.format) || !matches(motion,DXGI_FORMAT_R32G32_FLOAT) || !matches(depth,DXGI_FORMAT_R32_FLOAT) || color==s.output.Get())return E_INVALIDARG;
    ComPtr<ID3D12Device> listDevice;
    if(FAILED(list->GetDevice(IID_PPV_ARGS(&listDevice))) || listDevice.Get()!=s.device.Get())return E_INVALIDARG;
    if(!Complete(s.hostFence.Get(),s.hostValue))return S_FALSE;
    s.recording=true;s.heapIndex=0;s.capture=-1;s.hostInputs={color,motion,depth};
    ++s.stats.frame;enabled=enabled && s.config.enabled;
    if(reset || enabled!=s.enabledLast) {
        ++s.stats.generation;s.display=-1;
        for(auto& slot:s.slots)if(slot.phase!=State::Evaluating) {if(slot.phase!=State::Free)++s.stats.dropped;slot.phase=State::Free;}
    }
    s.enabledLast=enabled;
    for(auto& slot:s.slots)if(slot.phase==State::Ready && (slot.generation!=s.stats.generation || s.stats.frame-slot.frame>s.config.maxAge)) {
        slot.phase=State::Free;++s.stats.dropped;
    }
    int newest=s.display;
    if(enabled)for(unsigned i=0;i<s.slots.size();++i)if(s.slots[i].phase==State::Ready &&
        (newest<0 || s.slots[i].frame>s.slots[newest].frame))newest=int(i);
    if(newest!=s.display) {
        if(s.display>=0)s.slots[s.display].phase=State::Free;
        s.display=newest;if(newest>=0)s.slots[newest].phase=State::Displayed;
    }
    for(auto& slot:s.slots)if(slot.phase==State::Ready) {slot.phase=State::Free;++s.stats.dropped;}
    if(s.display>=0 && s.stats.frame-s.slots[s.display].frame>s.config.maxAge) {
        s.slots[s.display].phase=State::Free;s.display=-1;
    }
    try {
        State::Constants constants{s.config.width,s.config.height,0,0,s.config.motionScaleX,s.config.motionScaleY,
            s.config.depthTolerance,s.config.colorTolerance,s.config.maxRatio,0,s.config.maxAge};
        // Advance each immutable capture's map, including captures still being
        // evaluated. The worker never accesses these main-queue-only textures.
        if(enabled)for(auto& slot:s.slots)if(slot.phase!=State::Free) {
            auto next=1-slot.flowIndex;
            s.Dispatch(list,0,constants,{motion,slot.flow[slot.flowIndex].Get(),depth,s.previousDepth.Get()},slot.flow[next].Get());
            slot.flowIndex=next;
        }
        if(s.display>=0) {
            auto& slot=s.slots[s.display];constants.useResult=1;constants.age=unsigned(s.stats.frame-slot.frame);
            s.Dispatch(list,1,constants,{color,slot.flow[slot.flowIndex].Get(),slot.color.Get(),slot.output.Get(),depth,slot.depth.Get()},s.output.Get());
            s.stats.resultFrame=slot.frame;s.stats.resultAge=constants.age;s.stats.displaying=true;
        }else {Copy(list,color,s.output.Get());s.stats.displaying=false;s.stats.resultAge=0;s.stats.resultFrame=0;}
        // A single pending capture, replaced with the newest frame when its
        // preceding host submission is retired. Never build an inference backlog.
        if(enabled) {
            for(unsigned i=0;i<s.slots.size();++i)if(s.slots[i].phase==State::Pending){s.capture=int(i);++s.stats.dropped;break;}
            if(s.capture<0)for(unsigned i=0;i<s.slots.size();++i)if(s.slots[i].phase==State::Free){s.capture=int(i);break;}
            if(s.capture>=0) {
                auto& slot=s.slots[s.capture];slot.phase=State::Pending;slot.frame=s.stats.frame;slot.generation=s.stats.generation;
                slot.captureFence.Reset();slot.captureValue=0;slot.flowIndex=0;
                Copy(list,color,slot.color.Get());Copy(list,motion,slot.motion.Get());Copy(list,depth,slot.depth.Get());
                constants.initialize=1;s.Dispatch(list,0,constants,{},slot.flow[0].Get());
            }else ++s.stats.dropped;
        }
        Copy(list,depth,s.previousDepth.Get());
    }catch(const std::exception& error) {s.stats.failed=true;s.stats.error=error.what();return E_FAIL;}
    return S_OK;
}
HRESULT AsyncPipeline::Submitted(ID3D12Fence* fence,std::uint64_t value) {
    if(!state_ || !fence || !value || value==UINT64_MAX)return E_INVALIDARG;
    auto& s=*state_;std::lock_guard lock(s.mutex);
    ComPtr<ID3D12Device> owner;
    if(FAILED(fence->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=s.device.Get())return E_INVALIDARG;
    if(!s.recording || s.stats.failed || (s.hostFence && (s.hostFence.Get()!=fence || value<=s.hostValue)))return E_INVALIDARG;
    s.hostFence=fence;s.hostValue=value;s.recording=false;
    if(s.capture>=0) {auto& slot=s.slots[s.capture];slot.captureFence=fence;slot.captureValue=value;}
    s.wake.notify_one();return S_OK;
}
ID3D12Resource* AsyncPipeline::Output() const {return state_?state_->output.Get():nullptr;}
AsyncPipeline::Snapshot AsyncPipeline::Status() const {
    if(!state_)return {};std::lock_guard lock(state_->mutex);return state_->stats;
}
bool AsyncPipeline::Stop() {
    if(!state_)return true;auto& s=*state_;
    {std::lock_guard lock(s.mutex);s.stopping=true;s.wake.notify_one();}
    if(s.worker.joinable())s.worker.join();
    // workerOutstanding remains true on failed Signal/timeout/device removal.
    s.stopped=!s.recording && Complete(s.hostFence.Get(),s.hostValue) && !s.workerOutstanding;
    if(s.stopped)s.evaluate={}; // Release features only after both queues retire.
    return s.stopped;
}
}
