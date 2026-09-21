// Standalone NR bottleneck investigation; never launches or attaches to Skyrim.
#include "PCH.h"
#include "LaunchObserver.h"
#include "FrameGen/NeuralRenderingFeatureSession.h"
#include <nvsdk_ngx.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <cmath>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace NR = TheosRenderPipeline::NeuralRendering;

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
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 serial{}, frequency{};
    Context() {
        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc));
            if (desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
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
        check(queue->Signal(fence.Get(), ++serial)); check(fence->SetEventOnCompletion(serial, event));
        // A failed GPU wait is terminal. Keep submitted resources until process termination.
        if (WaitForSingleObject(event, 60000) != WAIT_OBJECT_0 || fence->GetCompletedValue() == UINT64_MAX) {
            std::cerr << "GPU work did not retire; terminating reference process" << std::endl;
            TerminateProcess(GetCurrentProcess(), 71);
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

int wmain(int argc, wchar_t** argv) try {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 11) {
        std::cerr << "TRPNRBottleneck <DLL> <input-dir> <output-dir> <width> <height> <frames> <reset-every> <style> <intensity> <baseline|observe|reuse>\n"; return 64;
    }
    auto dll = fs::absolute(argv[1]); auto inputs = fs::absolute(argv[2]); auto outputs = fs::absolute(argv[3]);
    UINT width = std::stoul(argv[4]), height = std::stoul(argv[5]), frames = std::stoul(argv[6]);
    UINT resetEvery = std::stoul(argv[7]); int style = std::stoi(argv[8]); float intensity = std::stof(argv[9]);
    require(width >= 64 && height >= 64 && width <= 2048 && height <= 2048 && frames > 0 && frames <= 256, "Invalid dimensions/frame count");
    require(style >= 0 && style <= 7 && std::isfinite(intensity) && intensity >= 0 && intensity <= 2, "Invalid controls");
    const std::wstring mode = argv[10];
    require(mode == L"baseline" || mode == L"observe" || mode == L"reuse", "Invalid probe mode");
    require(!fs::exists(outputs), "Output directory already exists");
    fs::create_directories(outputs);
    std::unique_ptr<LaunchObserver> observer;
    if (mode != L"baseline") observer = std::make_unique<LaunchObserver>(dll, outputs);
    Context c;
    // Bootstrap the normal NGX loader, as the production host does before the feature session.
    auto cache = outputs / "ngx-cache"; fs::create_directories(cache);
    auto init = NVSDK_NGX_D3D12_Init_with_ProjectID("f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "nr-recovery-oracle", cache.c_str(), c.device.Get(), nullptr, NVSDK_NGX_Version_API);
    require(NVSDK_NGX_SUCCEED(init) || init == NVSDK_NGX_Result_FAIL_FeatureAlreadyExists, "NGX bootstrap failed");
    {
        NR::FeatureSession feature; NR::FeatureSession::CreateInfo info{};
        info.device = c.device.Get(); info.runtimePath = dll; info.displayWidth = info.renderWidth = width;
        info.displayHeight = info.renderHeight = height; info.networkPreset = 0; info.allowReconstructionRuntime = true;
        require(feature.EnsureInitialized(info), feature.Status().c_str());
        auto color = c.texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto motion = c.texture(width, height, DXGI_FORMAT_R16G16_FLOAT);
        auto depth = c.texture(width, height, DXGI_FORMAT_R32_FLOAT);
        auto output = c.texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto backbuffer = c.texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        D3D12_QUERY_HEAP_DESC q{}; q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; q.Count = 2;
        ComPtr<ID3D12QueryHeap> queries; check(c.device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)));
        auto timings = c.buffer(16, D3D12_HEAP_TYPE_READBACK);
        UINT64 readBytes{}; auto layout = c.footprint(output.Get(), readBytes);
        auto readback = c.buffer(readBytes, D3D12_HEAP_TYPE_READBACK);
        std::ofstream csv(outputs / "timings.csv"); csv << "frame,reset,reused,evaluate_gpu_ms,upload_evaluate_readback_cpu_ms\n";
        for (UINT frame = 0; frame < frames; ++frame) {
            auto rgba = read(inputs / std::format("{:03}.rgba16", frame), size_t(width)*height*8);
            auto mv = read(inputs / std::format("{:03}.motion16", frame), size_t(width)*height*4);
            std::vector<float> depths(size_t(width)*height, 0.5f);
            std::vector<char> depthBytes(depths.size()*sizeof(float)); std::memcpy(depthBytes.data(), depths.data(), depthBytes.size());
            auto start = std::chrono::steady_clock::now(); c.begin();
            auto upColor = c.upload(color.Get(), rgba, size_t(width)*8);
            auto upMotion = c.upload(motion.Get(), mv, size_t(width)*4);
            auto upDepth = c.upload(depth.Get(), depthBytes, size_t(width)*4);
            auto upBack = c.upload(backbuffer.Get(), rgba, size_t(width)*8);
            NR::FeatureSession::EvaluationInput input{};
            input.commandList = c.list.Get(); input.color = color.Get(); input.motionVectors = motion.Get();
            input.depth = depth.Get(); input.output = output.Get(); input.backbuffer = backbuffer.Get();
            input.motionVectorScaleX = input.motionVectorScaleY = 1;
            input.reset = frame == 0 || (resetEvery && frame % resetEvery == 0);
            input.tuning.style = style; input.tuning.intensity = intensity; input.tuning.skinStructureStrength = -1;
            c.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            if (observer) observer->Begin(frame, input.reset, mode == L"reuse");
            require(feature.RecordEvaluation(input), feature.Status().c_str());
            if (observer) observer->End();
            c.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            c.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timings.Get(), 0);
            c.transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = readback.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = layout;
            src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            c.transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON); c.finish();
            if (observer) observer->Retired();
            auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
            void* mapped{}; D3D12_RANGE range{0, 16}; check(timings->Map(0, &range, &mapped));
            auto* timestamps = static_cast<UINT64*>(mapped); double gpuMs = double(timestamps[1]-timestamps[0])*1000/double(c.frequency);
            timings->Unmap(0, nullptr);
            range = {0, size_t(readBytes)}; check(readback->Map(0, &range, &mapped));
            std::vector<char> pixels(size_t(width)*height*8);
            for (UINT y = 0; y < height; ++y) std::memcpy(pixels.data() + y*size_t(width)*8, static_cast<char*>(mapped) + y*layout.Footprint.RowPitch, size_t(width)*8);
            readback->Unmap(0, nullptr);
            write(outputs / std::format("{:03}.rgba16", frame), pixels.data(), pixels.size());
            csv << frame << ',' << int(input.reset) << ',' << int(observer && observer->Reusing()) << ',' << gpuMs << ',' << elapsed << std::endl;
            std::cerr << "frame=" << frame << " gpu_ms=" << gpuMs << std::endl;
        }
        std::cerr << "All readbacks retired; releasing feature" << std::endl;
    }
    std::cerr << "Feature released; shutting down normal NGX loader" << std::endl;
    const auto shutdown = NVSDK_NGX_D3D12_Shutdown1(c.device.Get());
    require(NVSDK_NGX_SUCCEED(shutdown), "NGX shutdown failed");
    std::cerr << "Reference finished cleanly" << std::endl;
    return 0;
} catch (const std::exception& e) { std::cerr << "ERROR: " << e.what() << std::endl; return 1; }
