#pragma once
#include "FrameGen/SourceDLSSGNeuralRendering.h"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace TheosRenderPipeline::SourceDLSSG;
using Microsoft::WRL::ComPtr;
using Pixel=std::array<float,4>;
static void Require(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
static void Check(HRESULT hr,const char* why) {if(FAILED(hr)){std::fprintf(stderr,"FAIL: %s 0x%08lX\n",why,(unsigned long)hr);std::exit(1);}}
static void Near(double a,double b,double tolerance,const char* why) {
    if(std::abs(a-b)>tolerance || !std::isfinite(a)) {
        std::fprintf(stderr,"FAIL: %s actual=%.9f expected=%.9f tolerance=%.9f\n",why,a,b,tolerance);std::exit(1);
    }
}
struct GPU {
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence; ComPtr<ID3D12InfoQueue> info;
    HANDLE event{}; UINT64 serial{};
    explicit GPU(bool hardware=false) {
        // Pixel/state fixtures require validation. Hardware timings intentionally
        // exclude debug-layer overhead and use the normal game device mode.
        if(!hardware){ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug interface");debug->EnableDebugLayer();}
        ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter> adapter;
        if(hardware) Check(factory->EnumAdapters(0,&adapter),"hardware adapter");
        else Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");
        Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"D3D12 device");
        if(hardware){DXGI_ADAPTER_DESC a{};Check(adapter->GetDesc(&a),"adapter description");std::printf("GPU vendor=%04X device=%04X memory=%llu\n",a.VendorId,a.DeviceId,(unsigned long long)a.DedicatedVideoMemory);Require(a.VendorId==0x10de,"vendor benchmark needs NVIDIA adapter");}
        if(!hardware)Check(device.As(&info),"debug messages");
        D3D12_COMMAND_QUEUE_DESC d{};d.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&d,IID_PPV_ARGS(&queue)),"queue");
        Check(device->CreateCommandAllocator(d.Type,IID_PPV_ARGS(&allocator)),"allocator");
        Check(device->CreateCommandList(0,d.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
        Check(list->Close(),"initial close");Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"fence event");
    }
    ~GPU(){CloseHandle(event);}
    void Begin(){Check(allocator->Reset(),"retired allocator reset");Check(list->Reset(allocator.Get(),nullptr),"list reset");}
    void End(){
        Check(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        Check(queue->Signal(fence.Get(),++serial),"signal");Check(fence->SetEventOnCompletion(serial,event),"completion event");
        Require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0 && fence->GetCompletedValue()==serial,"GPU retirement");
        for(UINT64 i=0;info && i<info->GetNumStoredMessages();++i){
            SIZE_T n=0;Check(info->GetMessage(i,nullptr,&n),"message size");std::vector<char> bytes(n);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());Check(info->GetMessage(i,m,&n),"message");
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::fprintf(stderr,"D3D12: %s\n",m->pDescription);Require(false,"D3D12 validation");}
        }
        if(info)info->ClearStoredMessages();
    }
    void Barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&v);
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 size,D3D12_HEAP_TYPE type){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;
        d.Height=d.SampleDesc.Count=d.DepthOrArraySize=d.MipLevels=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES h{};h.Type=type;ComPtr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?
            D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;
    }
    static unsigned Components(DXGI_FORMAT format){return format==DXGI_FORMAT_R32_FLOAT?1:format==DXGI_FORMAT_R32G32_FLOAT?2:4;}
    ComPtr<ID3D12Resource> Texture(UINT w,UINT h,const std::vector<Pixel>& pixels={},DXGI_FORMAT format=DXGI_FORMAT_R32G32B32A32_FLOAT){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"texture");
        if(!pixels.empty()){
            Require(pixels.size()==size_t(w)*h,"upload extent");D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 size{};
            device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&size);auto upload=Buffer(size,D3D12_HEAP_TYPE_UPLOAD);
            void* data{};D3D12_RANGE noRead{};Check(upload->Map(0,&noRead,&data),"upload map");
            const auto c=Components(format);
            for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){
                if(format==DXGI_FORMAT_R16G16B16A16_FLOAT){
                    auto* pixel=reinterpret_cast<DirectX::PackedVector::HALF*>((char*)data+y*layout.Footprint.RowPitch+x*8);
                    for(unsigned ch=0;ch<4;++ch)pixel[ch]=DirectX::PackedVector::XMConvertFloatToHalf(pixels[y*w+x][ch]);
                }else std::memcpy((char*)data+y*layout.Footprint.RowPitch+x*c*4,pixels[y*w+x].data(),c*4);
            }
            upload->Unmap(0,nullptr);Begin();Barrier(r.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=layout;
            dst.pResource=r.Get();list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            Barrier(r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);End();
        }
        return r;
    }
    std::vector<Pixel> Read(ID3D12Resource* r){
        const auto d=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 size{};
        device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&size);auto readback=Buffer(size,D3D12_HEAP_TYPE_READBACK);
        Begin();Barrier(r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=r;dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=layout;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(r,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);End();
        void* data{};D3D12_RANGE range{0,size_t(size)};Check(readback->Map(0,&range,&data),"read map");
        std::vector<Pixel> pixels(size_t(d.Width)*d.Height);const auto c=Components(d.Format);
        for(UINT y=0;y<d.Height;++y)for(UINT x=0;x<d.Width;++x){
            if(d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT){
                const auto* pixel=reinterpret_cast<DirectX::PackedVector::HALF*>((char*)data+y*layout.Footprint.RowPitch+x*8);
                for(unsigned ch=0;ch<4;++ch)pixels[y*d.Width+x][ch]=DirectX::PackedVector::XMConvertHalfToFloat(pixel[ch]);
            }else std::memcpy(pixels[y*d.Width+x].data(),(char*)data+y*layout.Footprint.RowPitch+x*c*4,c*4);
        }
        D3D12_RANGE noWrite{};readback->Unmap(0,&noWrite);return pixels;
    }
};

