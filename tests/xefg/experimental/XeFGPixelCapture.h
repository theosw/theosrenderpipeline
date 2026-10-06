#pragma once
// Deliberately perturbing, bounded readback. Never enabled in timing runs.
#include "../XeFGTestSupport.h"
#include "XeFGUnlock.h"
#include <fstream>
#include <mutex>

struct XeFGPixelCapture {
    std::filesystem::path directory;
    unsigned saved{};
    unsigned generatedFrames{3};
    bool started{};
    std::mutex mutex;
    static inline XeFGPixelCapture* active{};
    // Pinned 1.3.1.78 ABI: copy helper 0x21d3c0 uses context+0x10 as
    // ID3D12CommandQueue and +0x18 as IDXGISwapChain. It submits the copy
    // and restores the destination to PRESENT before calling our thunk.
    // Install's exact file hash and whole mapped .text check admit this ABI.
    static bool Targets(std::uintptr_t context, IUnknown*& q, IUnknown*& s) {
        __try {
            q=*reinterpret_cast<IUnknown**>(context+0x10);
            s=*reinterpret_cast<IUnknown**>(context+0x18);
            return q && s;
        } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    }
    static void Observe(const XeFGExperiment::Event& e) noexcept {
        if(active) active->Capture(e);
    }
    void Capture(const XeFGExperiment::Event& e) {
        std::lock_guard lock(mutex);
        if(saved>=3*generatedFrames || e.count!=generatedFrames) return;
        if(!started) { if(e.index!=1 || e.deadlineNs==0) return; started=true; }
        IUnknown *queueRaw{}, *chainRaw{};
        Require(Targets(e.context,queueRaw,chainRaw),"pinned capture ABI");
        ComPtr<ID3D12CommandQueue> queue; ComPtr<IDXGISwapChain3> chain;
        Check(queueRaw->QueryInterface(IID_PPV_ARGS(&queue)),"capture provider queue");
        Check(chainRaw->QueryInterface(IID_PPV_ARGS(&chain)),"capture native swapchain");
        ComPtr<ID3D12Resource> back; ComPtr<ID3D12Device> device;
        Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"pre-Present native buffer");
        Check(back->GetDevice(IID_PPV_ARGS(&device)),"capture device");
        const auto desc=back->GetDesc();
        Require(desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM && desc.SampleDesc.Count==1,"capture RGBA8 target");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows{}; UINT64 rowBytes{}, size{};
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&size);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=size; buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1;
        buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"capture readback");
        ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"capture allocator");
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"capture list");
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={back.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=back.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); list->ResourceBarrier(1,&barrier);
        Check(list->Close(),"capture close");
        ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"capture retirement fence");
        HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(done!=nullptr,"capture event");
        Check(queue->Signal(fence.Get(),1),"capture signal");
        Check(fence->SetEventOnCompletion(1,done),"capture completion");
        Require(WaitForSingleObject(done,10000)==WAIT_OBJECT_0,"capture retirement before release"); CloseHandle(done);
        void* mapped{}; D3D12_RANGE range{0,static_cast<SIZE_T>(size)};
        Check(readback->Map(0,&range,&mapped),"capture map");
        const auto path=directory/("frame-"+std::to_string(saved)+"-index-"+std::to_string(e.index)+".ppm");
        std::ofstream file(path,std::ios::binary); file<<"P6\n"<<desc.Width<<' '<<desc.Height<<"\n255\n";
        const auto* bytes=static_cast<const unsigned char*>(mapped)+footprint.Offset;
        for(UINT y=0;y<desc.Height;++y) for(UINT x=0;x<desc.Width;++x)
            file.write(reinterpret_cast<const char*>(bytes+y*footprint.Footprint.RowPitch+x*4),3);
        Require(file.good(),"write pixel evidence");
        D3D12_RANGE written{0,0}; readback->Unmap(0,&written);
        std::printf("pixel_capture n=%u index=%llu size=%llux%u path=%s timingPerturbed=true\n",saved,e.index,desc.Width,desc.Height,path.string().c_str());
        ++saved;
    }
};
