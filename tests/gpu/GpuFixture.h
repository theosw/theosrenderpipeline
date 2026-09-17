#pragma once
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
using Pixel = std::array<float,4>;
inline void Require(bool ok, const char* why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1); } }
inline void Check(HRESULT hr,const char* why) { if(FAILED(hr)) {std::fprintf(stderr,"FAIL: %s 0x%08X\n",why,unsigned(hr));std::exit(1);} }
struct GPU {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;ComPtr<ID3D12InfoQueue> info;HANDLE event{};UINT64 serial{};
    GPU() {
        ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug");debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter> adapter;Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP");
        Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"device");Check(device.As(&info),"info");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"allocator");
        Check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");Check(list->Close(),"close");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"event");
    }
    ~GPU(){CloseHandle(event);}
    void Begin(){Check(allocator->Reset(),"retired allocator reset");Check(list->Reset(allocator.Get(),nullptr),"list reset");}
    void End(){Check(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        Check(queue->Signal(fence.Get(),++serial),"signal");Check(fence->SetEventOnCompletion(serial,event),"completion");
        Require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"retire");}
    void Barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);}
    ComPtr<ID3D12Resource> Buffer(UINT64 size,D3D12_HEAP_TYPE type){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=d.SampleDesc.Count=1;
        d.DepthOrArraySize=d.MipLevels=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES h{};h.Type=type;ComPtr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?
            D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;}
    ComPtr<ID3D12Resource> Texture(UINT w,UINT h,Pixel value){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)),"texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 size{};device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&size);
        auto upload=Buffer(size,D3D12_HEAP_TYPE_UPLOAD);void* mapped{};D3D12_RANGE noRead{};Check(upload->Map(0,&noRead,&mapped),"upload map");
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)std::memcpy(static_cast<char*>(mapped)+y*layout.Footprint.RowPitch+x*sizeof(Pixel),value.data(),sizeof(Pixel));
        upload->Unmap(0,nullptr);Begin();Barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=layout;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);End();return texture;}
    std::vector<Pixel> Read(ID3D12Resource* resource){
        const auto d=resource->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 size{};
        device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&size);auto readback=Buffer(size,D3D12_HEAP_TYPE_READBACK);
        Begin();Barrier(resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=resource;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=layout;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);End();
        void* mapped{};D3D12_RANGE range{0,SIZE_T(size)};Check(readback->Map(0,&range,&mapped),"readback");std::vector<Pixel> pixels(size_t(d.Width)*d.Height);
        for(UINT y=0;y<d.Height;++y)std::memcpy(pixels.data()+y*d.Width,static_cast<char*>(mapped)+y*layout.Footprint.RowPitch,size_t(d.Width)*sizeof(Pixel));
        D3D12_RANGE written{};readback->Unmap(0,&written);return pixels;}
    void CheckDebug(){for(UINT64 i=0;i<info->GetNumStoredMessages();++i){
        SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        info->GetMessage(i,msg,&size);Require(msg->Severity>D3D12_MESSAGE_SEVERITY_ERROR,msg->pDescription);}}
};
