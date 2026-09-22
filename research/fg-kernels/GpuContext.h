#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <format>
#include <iostream>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error(std::format("D3D failure 0x{:08X}", unsigned(hr))); }
std::vector<char> read(const fs::path& path, size_t expected) {
    require(fs::file_size(path) == expected, "Input size mismatch");
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes(expected);
    in.read(bytes.data(), bytes.size());
    require(bool(in), "Input read failed"); return bytes;
}
void write(const fs::path& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), size);
    require(bool(out), "Output write failed");
}
struct Context {
    ComPtr<ID3D12InfoQueue> messages;
    std::string adapterIdentity;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 serial{}, frequency{};
    Context() {
        if(GetEnvironmentVariableW(L"TRP_FG_DEBUG",nullptr,0)){
            ComPtr<ID3D12Debug> debug;check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
        }
        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc));
            if (desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                LARGE_INTEGER driver{};check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver));
                adapterIdentity=std::format("vendor={:04X} device={:04X} luid={:08X}:{:08X} driver={}.{}.{}.{}",desc.VendorId,desc.DeviceId,unsigned(desc.AdapterLuid.HighPart),desc.AdapterLuid.LowPart,
                    HIWORD(driver.HighPart),LOWORD(driver.HighPart),HIWORD(driver.LowPart),LOWORD(driver.LowPart));
                device.As(&messages);
                std::wcerr << L"adapter=" << desc.Description << std::endl; break;
            }
        }
        require(bool(device), "No NVIDIA D3D12 adapter");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        check(list->Close());
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "Event creation failed");
        check(queue->GetTimestampFrequency(&frequency));
    }
    ~Context() { if (event) CloseHandle(event); }
    void begin() { check(allocator->Reset()); check(list->Reset(allocator.Get(), nullptr)); }
    void finish() {
        check(list->Close()); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        // Submission transfers ownership to the GPU. Failed signaling/wait setup
        // is terminal too: unwinding must not release possibly referenced resources.
        if(FAILED(queue->Signal(fence.Get(), ++serial)) || FAILED(fence->SetEventOnCompletion(serial,event))){
            std::cerr<<"GPU retirement could not be established; terminating probe"<<std::endl;
            TerminateProcess(GetCurrentProcess(),71);
        }
        // A failed GPU wait is terminal. Keep submitted resources until process termination.
        if (WaitForSingleObject(event, 60000) != WAIT_OBJECT_0 || fence->GetCompletedValue() == UINT64_MAX) {
            std::cerr << "GPU work did not retire; terminating reference process" << std::endl;
            TerminateProcess(GetCurrentProcess(), 71);
        }
        if(messages){
            bool failed=false;
            for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
                SIZE_T size{};check(messages->GetMessage(i,nullptr,&size));std::vector<char> bytes(size);
                auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());check(messages->GetMessage(i,m,&size));
                if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<m->pDescription<<std::endl;failed=true;}
            }
            messages->ClearStoredMessages();require(!failed,"D3D12 validation failed");
        }
    }
    ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&r))); return r;
    }
    ComPtr<ID3D12Resource> texture(UINT w, UINT h, DXGI_FORMAT format) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Format = format; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r))); return r;
    }
    void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; list->ResourceBarrier(1, &b);
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint(ID3D12Resource* r, UINT64& bytes) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{}; auto desc = r->GetDesc();
        device->GetCopyableFootprints(&desc, 0, 1, 0, &f, nullptr, nullptr, &bytes); return f;
    }
    ComPtr<ID3D12Resource> upload(ID3D12Resource* r, const std::vector<char>& bytes, size_t stride) {
        UINT64 size{}; auto f = footprint(r, size); auto staging = buffer(size, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{}; D3D12_RANGE noRead{}; check(staging->Map(0, &noRead, &mapped));
        for (UINT y = 0; y < f.Footprint.Height; ++y) std::memcpy(static_cast<char*>(mapped) + y*f.Footprint.RowPitch, bytes.data() + y*stride, stride);
        staging->Unmap(0, nullptr);
        transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = r;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = staging.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = f;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON); return staging;
    }
};
