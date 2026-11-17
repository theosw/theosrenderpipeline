// Standalone FFX API fixture. No game, bridge, settings, NVIDIA runtime or weights.
#include "src/FrameGen/FSR4Compatibility.h"
#include "extern/fidelityfx-api/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "extern/fidelityfx-api/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include "DispatchTrace.h"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <DirectXPackedVector.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace Compat = TheosRenderPipeline::FSR4Compatibility;
namespace
{
    void Require(bool value, const char* message)
    {
        // Process containment also retains potentially in-flight resources on failure.
        if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::fflush(nullptr); ExitProcess(1); }
    }
    void Check(HRESULT result, const char* message)
    {
        if (FAILED(result)) { std::fprintf(stderr, "HRESULT=%08x ", unsigned(result)); Require(false, message); }
    }
    void Ffx(ffxReturnCode_t result, const char* message)
    {
        if (result) { std::fprintf(stderr, "FFX=%u ", result); Require(false, message); }
    }
    float Halton(unsigned i, unsigned base)
    {
        float value = 0, step = 1; for (; i; i /= base) { step /= base; value += step * (i % base); } return value - .5f;
    }
    template<class T> T Read(uintptr_t base, size_t offset = 0)
    {
        T value{}; Require(Compat::ReadMemory(reinterpret_cast<void*>(base + offset), &value, sizeof(value)), "bounded identity observation"); return value;
    }
    std::mutex allocationMutex;
    std::map<void*, uint64_t> allocations;
    void* Allocate(void*, uint64_t n)
    {
        auto* p = std::calloc(1, static_cast<size_t>(n));
        if (p) { std::lock_guard lock(allocationMutex); allocations[p] = n; } return p;
    }
    void Deallocate(void*, void* p)
    {
        { std::lock_guard lock(allocationMutex); allocations.erase(p); } std::free(p);
    }
    void Message(uint32_t type, const wchar_t* message) { std::printf("FFX_MESSAGE type=%u %ls\n", type, message); }
    void DebugMessages(ID3D12Device* device)
    {
        ComPtr<ID3D12InfoQueue> messages; Check(device->QueryInterface(IID_PPV_ARGS(&messages)), "debug info queue");
        const auto count=messages->GetNumStoredMessagesAllowedByRetrievalFilter(); unsigned errors{};
        for (UINT64 i=0;i<count;++i) {
            SIZE_T bytes{}; Check(messages->GetMessage(i,nullptr,&bytes), "debug message size");
            std::vector<BYTE> storage(bytes); auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(messages->GetMessage(i,m,&bytes), "debug message");
            std::printf("D3D12_MESSAGE severity=%u id=%u %s\n",m->Severity,m->ID,m->pDescription);
            if (m->Severity==D3D12_MESSAGE_SEVERITY_ERROR || m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) ++errors;
        }
        std::printf("D3D12_DEBUG messages=%llu errors=%u\n",count,errors);
        Require(errors==0,"D3D12 validation errors");
    }
    struct Identity { bool found{}, built{}; unsigned preset{}; uintptr_t context{}, provider{}, model{}; };
    Identity Observe(HMODULE upscaler, unsigned generation, const char* stage)
    {
        Identity result;
        const auto base = reinterpret_cast<uintptr_t>(upscaler);
        std::lock_guard lock(allocationMutex);
        // No heap/weight dump: only allocation headers that can own the pinned
        // full-INT8 provider, then specific scalar fields derived from disassembly.
        for (const auto& [p, size] : allocations) {
            if (size < 0x21e) continue;
            const auto context = reinterpret_cast<uintptr_t>(p);
            const auto provider = Read<uintptr_t>(context);
            uintptr_t vtable{};
            if (!Compat::ReadMemory(reinterpret_cast<void*>(provider), &vtable, sizeof(vtable)) || vtable != base + 0x3635260) continue;
            const auto model = Read<uintptr_t>(provider, 0x28);
            if (Read<uintptr_t>(model) != base + 0x36352a8 || Read<uint64_t>(provider, 0x10) != 0xf5a5ca1e01001002ull) continue;
            Require(!result.found, "ambiguous INT8 context allocation");
            result = {true, Read<unsigned char>(context, 0x214) != 0, Read<unsigned>(context, 0x208), context, provider, model};
        }
        std::printf("IDENTITY generation=%u stage=%s found=%d context=%p provider=%p provider_vtable_rva=3635260 model=%p model_vtable_rva=36352a8 type=Fsr4Int8UpscalerModel lazy_built=%d preset=%u\n",
            generation, stage, result.found, reinterpret_cast<void*>(result.context), reinterpret_cast<void*>(result.provider),
            reinterpret_cast<void*>(result.model), result.built, result.preset);
        return result;
    }
    struct GPU
    {
        ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence; HANDLE event{}; UINT64 serial{}, frequency{};
        ComPtr<ID3D12QueryHeap> queries; ComPtr<ID3D12Resource> timestamps;
        ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type)
        {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
            D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
            d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> result;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&result)), "buffer"); return result;
        }
        ComPtr<ID3D12Resource> Texture(UINT w, UINT h, DXGI_FORMAT format)
        {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
            d.DepthOrArraySize = d.MipLevels = 1; d.Format = format; d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> result;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, IID_PPV_ARGS(&result)), "texture"); return result;
        }
        void Init(IDXGIAdapter1* adapter)
        {
            Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12 device");
            D3D12_COMMAND_QUEUE_DESC q{}; Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
            Check(list->Close(), "initial close"); Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "fence event");
            Check(queue->GetTimestampFrequency(&frequency), "timestamp frequency"); Require(frequency != 0, "nonzero frequency");
            D3D12_QUERY_HEAP_DESC query{}; query.Count = 2; query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            Check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&queries)), "timestamp query heap"); timestamps = Buffer(16, D3D12_HEAP_TYPE_READBACK);
            std::printf("GPU_TIMESTAMP frequency=%llu\n", frequency);
        }
        void Begin() { Check(allocator->Reset(), "allocator reset after completed fence"); Check(list->Reset(allocator.Get(), nullptr), "list reset"); }
        void Submit()
        {
            Check(list->Close(), "close"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), ++serial), "queue signal");
            Check(fence->SetEventOnCompletion(serial, event), "fence event registration");
            if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0) {
                std::printf("GPU_TIMEOUT_OR_LOSS removed_reason=%08x fence=%llu completed=%llu\n", unsigned(device->GetDeviceRemovedReason()), serial, fence->GetCompletedValue());
                std::fflush(nullptr); ExitProcess(124);
            }
            Require(fence->GetCompletedValue() >= serial && fence->GetCompletedValue() != UINT64_MAX, "completed fence without device removal");
            Check(device->GetDeviceRemovedReason(), "device health after fence");
        }
        double Milliseconds()
        {
            UINT64* data{}; const D3D12_RANGE read{0,16}; Check(timestamps->Map(0, &read, reinterpret_cast<void**>(&data)), "timestamps map");
            const auto start = data[0], end = data[1]; const D3D12_RANGE written{}; timestamps->Unmap(0, &written);
            Require(end >= start, "ordered timestamps"); return double(end - start) * 1000.0 / double(frequency);
        }
        ~GPU() { if (event) CloseHandle(event); }
    };
    void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = resource;
        b.Transition.StateBefore = from; b.Transition.StateAfter = to; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);
    }
    struct Scene
    {
        ComPtr<ID3D12RootSignature> root; ComPtr<ID3D12PipelineState> pipeline; ComPtr<ID3D12DescriptorHeap> heap;
        Scene(GPU& gpu, ID3D12Resource* const* textures)
        {
            static constexpr char source[] = R"(
cbuffer Parameters:register(b0) { uint2 inputSize; uint2 outputSize; float2 jitter; uint frame; uint padding; };
RWTexture2D<float4> color:register(u0); RWTexture2D<float2> motion:register(u1);
RWTexture2D<float> depth:register(u2); RWTexture2D<float2> exposure:register(u3);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=inputSize)) return;
 float2 uv=(id.xy+0.5-jitter)/inputSize; float u=uv.x,v=uv.y;
 float3 rgb=float3(0.12+0.5*u,0.16+0.35*v,0.18);
 if(u>0.52 && v<0.5) rgb=sin((u+v*0.7)*outputSize.x*1.1)>0?0.92:0.08;
 if(u<0.48 && v<0.45) rgb.g=0.25+0.5*pow(abs(sin(u*310)*sin(v*287)),3);
 if(v>0.7) rgb=0.5+0.4*sin((u*u+v*v)*outputSize.x*0.25);
 float position=0.28+0.08*sin(frame*0.025), previous=0.28+0.08*sin((int(frame)-1)*0.025);
 bool object=u>position && u<position+0.17 && v>0.38 && v<0.66;
 if(object) rgb=float3(0.85,0.16,0.07);
 color[id.xy]=float4(rgb,1); motion[id.xy]=float2(object?previous-position:0,0); depth[id.xy]=object?0.65:0.98;
 if(all(id.xy==0)) exposure[uint2(0,0)]=float2(1,0);
})";
            ComPtr<ID3DBlob> code, errors;
            const auto hr = D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer())); Check(hr, "scene compile");
            D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,0}; D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[0].DescriptorTable = {1,&range};
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[1].Constants = {0,0,8};
            D3D12_ROOT_SIGNATURE_DESC desc{2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE}; ComPtr<ID3DBlob> serialized;
            Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors), "scene root serialization");
            Check(gpu.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)), "scene root");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root.Get(); pd.CS = {code->GetBufferPointer(),code->GetBufferSize()};
            Check(gpu.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)), "scene PSO");
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            Check(gpu.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)), "scene descriptors");
            auto handle = heap->GetCPUDescriptorHandleForHeapStart(); const auto stride = gpu.device->GetDescriptorHandleIncrementSize(hd.Type);
            for (unsigned i = 0; i < 4; ++i) { gpu.device->CreateUnorderedAccessView(textures[i],nullptr,nullptr,handle); handle.ptr += stride; }
        }
        void Render(GPU& gpu, UINT iw, UINT ih, UINT ow, UINT oh, unsigned f, float jx, float jy)
        {
            struct Constants { UINT iw,ih,ow,oh; float jx,jy; UINT frame,padding; } constants{iw,ih,ow,oh,jx,jy,f,0};
            auto* list = gpu.list.Get(); list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pipeline.Get());
            ID3D12DescriptorHeap* heaps[]{heap.Get()}; list->SetDescriptorHeaps(1,heaps);
            list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart()); list->SetComputeRoot32BitConstants(1,8,&constants,0);
            list->Dispatch((iw+7)/8,(ih+7)/8,1);
        }
    };
    void Capture(GPU& gpu, ID3D12Resource* output, const std::filesystem::path& path)
    {
        const auto desc = output->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 total{};
        gpu.device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&total); auto readback = gpu.Buffer(total,D3D12_HEAP_TYPE_READBACK);
        gpu.Begin(); Barrier(gpu.list.Get(),output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = output; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource = readback.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; target.PlacedFootprint = layout;
        gpu.list->CopyTextureRegion(&target,0,0,0,&source,nullptr); Barrier(gpu.list.Get(),output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); gpu.Submit();
        BYTE* data{}; D3D12_RANGE range{0,static_cast<SIZE_T>(total)}; Check(readback->Map(0,&range,reinterpret_cast<void**>(&data)), "output readback");
        std::vector<BYTE> bgr(static_cast<size_t>(desc.Width)*desc.Height*3), raw(static_cast<size_t>(desc.Width)*desc.Height*8);
        size_t nonfinite{}, outside{}; double minimum = INFINITY, maximum = -INFINITY, sum = 0;
        for (UINT y=0; y<desc.Height; ++y) {
            const auto* row = data+layout.Offset+static_cast<size_t>(y)*layout.Footprint.RowPitch;
            memcpy(raw.data()+static_cast<size_t>(y)*desc.Width*8,row,static_cast<size_t>(desc.Width)*8);
            for (UINT x=0; x<desc.Width; ++x) for (unsigned c=0; c<4; ++c) {
                uint16_t bits{}; memcpy(&bits,row+x*8+c*2,2); const float v = DirectX::PackedVector::XMConvertHalfToFloat(bits);
                if (!std::isfinite(v)) ++nonfinite;
                if (c<3) {
                    minimum=std::min(minimum,double(v)); maximum=std::max(maximum,double(v)); sum+=v;
                    if (v<-.05f || v>1.5f) ++outside;
                    bgr[(static_cast<size_t>(y)*desc.Width+x)*3+(2-c)] = static_cast<BYTE>(std::clamp(v,0.f,1.f)*255+.5f);
                }
            }
        }
        const D3D12_RANGE written{}; readback->Unmap(0,&written);
        std::ofstream pixels(path.string()+".rgba16f",std::ios::binary); pixels.write(reinterpret_cast<const char*>(raw.data()),static_cast<std::streamsize>(raw.size())); pixels.close();
        std::printf("READBACK file=%s dimensions=%llux%u nonfinite=%zu outside=%zu rgb_min=%.6f rgb_max=%.6f mean=%.6f sha256=%s\n",
            path.string().c_str(),desc.Width,desc.Height,nonfinite,outside,minimum,maximum,sum/(desc.Width*desc.Height*3),Compat::Sha256(raw.data(),raw.size()).c_str());
        Require(!nonfinite && maximum-minimum>.1 && sum>0, "finite nonconstant output");
        ComPtr<IWICImagingFactory> factory; Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
        ComPtr<IWICStream> stream; Check(factory->CreateStream(&stream),"PNG stream"); Check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG file");
        ComPtr<IWICBitmapEncoder> encoder; Check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder"); Check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG init");
        ComPtr<IWICBitmapFrameEncode> frame; Check(encoder->CreateNewFrame(&frame,nullptr),"PNG frame"); Check(frame->Initialize(nullptr),"PNG frame init");
        Check(frame->SetSize(static_cast<UINT>(desc.Width),desc.Height),"PNG size"); auto format=GUID_WICPixelFormat24bppBGR; Check(frame->SetPixelFormat(&format),"PNG format");
        Require(format==GUID_WICPixelFormat24bppBGR,"PNG BGR"); Check(frame->WritePixels(desc.Height,static_cast<UINT>(desc.Width)*3,static_cast<UINT>(bgr.size()),bgr.data()),"PNG data");
        Check(frame->Commit(),"PNG frame commit"); Check(encoder->Commit(),"PNG commit");
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); std::setvbuf(stdout,nullptr,_IONBF,0);
    Require(argc==8,"usage: fixture AMD_DIR OUT_DIR auto|classic|force|bad-preimage|preloaded INPUT_W INPUT_H timing|proof|debug|recreate FRAMES");
    const std::filesystem::path amd=argv[1], out=argv[2]; const std::wstring mode=argv[3], run=argv[6];
    const UINT iw=static_cast<UINT>(_wtoi(argv[4])), ih=static_cast<UINT>(_wtoi(argv[5])), ow=iw*2, oh=ih*2;
    const unsigned measured=static_cast<unsigned>(_wtoi(argv[7]));
    Require((iw==1280 && ih==720)||(iw==1920 && ih==1080),"qualified fixed extents only"); Require(measured>0 && measured<=1000,"bounded frames");
    Require(mode==L"auto" || mode==L"classic" || mode==L"force" || mode==L"bad-preimage" || mode==L"preloaded","mode");
    Require(run==L"timing" || run==L"proof" || run==L"debug" || run==L"recreate","run kind");
    const bool force=mode==L"force" || mode==L"bad-preimage", proof=run==L"proof" || run==L"debug", debug=run==L"debug";
    _wputenv_s(L"MLSR-WATERMARK",proof?L"1":L""); Check(SetEnvironmentVariableW(L"MLSR-WATERMARK",proof?L"1":nullptr)?S_OK:E_FAIL,"watermark environment");
    std::filesystem::create_directories(out); Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
    std::printf("CASE mode=%ls run=%ls input=%ux%u output=%ux%u NR=off FG=off sharpening=off watermark=%d debug=%d pid=%lu\n",mode.c_str(),run.c_str(),iw,ih,ow,oh,proof,debug,GetCurrentProcessId());
    if (mode==L"preloaded") Require(LoadLibraryExW((amd/L"amd_fidelityfx_loader_dx12.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32)!=nullptr,"test preload");
    Compat::Session runtime;
    if (!runtime.LoadVerified(amd)) { std::printf("REFUSED %s\n%s",runtime.Reason().c_str(),runtime.Audit().c_str()); return 20; }
    std::printf("%s",runtime.Audit().c_str());
    if (debug) { ComPtr<ID3D12Debug> d; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&d)),"D3D12 debug layer"); d->EnableDebugLayer(); }
    ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
    ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 adapterDesc{};
    for (UINT i=0; ; ++i) {
        ComPtr<IDXGIAdapter1> candidate; Require(SUCCEEDED(factory->EnumAdapters1(i,&candidate)),"RTX 4080 SUPER required");
        DXGI_ADAPTER_DESC1 d{}; Check(candidate->GetDesc1(&d),"adapter desc");
        if (d.VendorId==0x10de && d.DeviceId==0x2702 && !(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter=candidate; adapterDesc=d; break; }
    }
    LARGE_INTEGER driver{}; Check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver),"driver version");
    std::printf("ADAPTER %ls vendor=%04x device=%04x luid=%08x:%08x driver=%llu\n",adapterDesc.Description,adapterDesc.VendorId,adapterDesc.DeviceId,adapterDesc.AdapterLuid.HighPart,adapterDesc.AdapterLuid.LowPart,driver.QuadPart);
    GPU gpu; gpu.Init(adapter.Get());
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6}; Check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm)),"real SM");
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{}; D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
    Check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,&o1,sizeof(o1)),"real wave capabilities");
    Check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4,&o4,sizeof(o4)),"real half capabilities");
    std::printf("FEATURES shader_model=%x waves=%d wave_min=%u wave_max=%u native16=%d\n",sm.HighestShaderModel,o1.WaveOps,o1.WaveLaneCountMin,o1.WaveLaneCountMax,o4.Native16BitShaderOpsSupported);
    if (mode==L"bad-preimage") {
        auto* byte=reinterpret_cast<BYTE*>(runtime.Upscaler())+0x99b5; DWORD old{},ignored{};
        Require(VirtualProtect(byte,1,PAGE_EXECUTE_READWRITE,&old),"test preimage protect"); *byte=0x90;
        Require(VirtualProtect(byte,1,old,&ignored) && FlushInstructionCache(GetCurrentProcess(),byte,1),"test preimage mutation");
    }
    const auto observedModule=runtime.Upscaler();
    if (force && !runtime.ApplyBeforeFirstQuery(gpu.device.Get(),adapterDesc.AdapterLuid,true)) {
        if (mode==L"bad-preimage") {
            unsigned char actual[5]{}; constexpr unsigned char injected[]{0x90,0x8d,0x54,0x24,0x20};
            Require(Compat::ReadMemory(reinterpret_cast<BYTE*>(observedModule)+0x99b5,actual,sizeof(actual)) &&
                memcmp(actual,injected,sizeof(actual))==0,"refusal must leave test preimage unchanged");
            std::printf("REFUSAL_READBACK 908d542420 unchanged; no replacement bytes written\n");
        }
        std::printf("REFUSED %s\n%s",runtime.Reason().c_str(),runtime.Audit().c_str()); return 20;
    }
    if (mode==L"bad-preimage") Require(false,"bad preimage was incorrectly accepted");
    std::printf("RUNTIME %s\n%s",runtime.Reason().c_str(),runtime.Audit().c_str());
    auto create=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(runtime.Loader(),"ffxCreateContext"));
    auto destroy=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(runtime.Loader(),"ffxDestroyContext"));
    auto query=reinterpret_cast<PfnFfxQuery>(GetProcAddress(runtime.Loader(),"ffxQuery"));
    auto dispatch=reinterpret_cast<PfnFfxDispatch>(GetProcAddress(runtime.Loader(),"ffxDispatch"));
    Require(create && destroy && query && dispatch,"FFX exports");
    DispatchTrace::Install(gpu.device.Get(),gpu.list.Get(),gpu.queue.Get(),out);
    uint64_t count{}; ffxQueryDescGetVersions versions{}; versions.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; versions.device=gpu.device.Get(); versions.outputCount=&count;
    Ffx(query(nullptr,&versions.header),"version count"); Require(count && count<=64,"bounded offered versions");
    std::vector<uint64_t> ids(count); std::vector<const char*> names(count); versions.versionIds=ids.data(); versions.versionNames=names.data();
    Ffx(query(nullptr,&versions.header),"version enumeration"); Require(count<=ids.size(),"version result bounds");
    uint64_t selected{};
    for (size_t i=0;i<count;++i) {
        std::printf("OFFERED id=%016llx name=%s\n",ids[i],names[i]?names[i]:"NULL");
        if (force && ids[i]==0xf5a5ca1e01001002ull && names[i] && std::string(names[i])=="4.1.2") selected=ids[i];
        if (mode==L"classic" && !selected && !(ids[i]&0x80000000ull) && names[i] && std::string(names[i]).starts_with("3.1.")) selected=ids[i];
    }
    Require((!force && mode!=L"classic") || selected,"requested SDK provider actually offered");
    std::printf("SELECTED override_id=%016llx\n",selected);
    auto color=gpu.Texture(iw,ih,DXGI_FORMAT_R16G16B16A16_FLOAT), motion=gpu.Texture(iw,ih,DXGI_FORMAT_R16G16_FLOAT),
        depth=gpu.Texture(iw,ih,DXGI_FORMAT_R32_FLOAT), exposure=gpu.Texture(1,1,DXGI_FORMAT_R32G32_FLOAT), output=gpu.Texture(ow,oh,DXGI_FORMAT_R16G16B16A16_FLOAT);
    ID3D12Resource* inputs[]{color.Get(),motion.Get(),depth.Get(),exposure.Get()}; Scene scene(gpu,inputs);
    ffxCreateBackendDX12Desc backend{}; backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device=gpu.device.Get();
    ffxCreateContextDescUpscaleVersion api{}; api.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION,&backend.header}; api.version=FFX_UPSCALER_VERSION;
    ffxOverrideVersion override{}; override.header={FFX_API_DESC_TYPE_OVERRIDE_VERSION,&api.header}; override.versionId=selected;
    ffxCreateContextDescUpscale desc{}; desc.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,selected?&override.header:&api.header};
    desc.maxRenderSize={iw,ih}; desc.maxUpscaleSize={ow,oh}; desc.flags=FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (proof) desc.flags|=FFX_UPSCALE_ENABLE_DEBUG_CHECKING; if (debug) desc.flags|=FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    desc.fpMessage=&Message; ffxAllocationCallbacks callbacks{nullptr,&Allocate,&Deallocate};
    std::ofstream timings(out/"timings.csv"); timings<<"context,frame,reset,fence,gpu_ms,measured\n";
    const unsigned warmup=run==L"timing"?120:8, generations=run==L"recreate"?2:1;
    for (unsigned generation=0;generation<generations;++generation) {
        ffxContext context{}; Ffx(create(&context,&desc.header,&callbacks),"create context");
        ffxQueryGetProviderVersion provider{}; provider.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        Ffx(query(&context,&provider.header),"selected provider query"); Require(provider.versionName,"provider name");
        std::printf("PROVIDER generation=%u id=%016llx name=%s context=%p\n",generation,provider.versionId,provider.versionName,context);
        Require(force ? provider.versionId==selected && std::string(provider.versionName)=="4.1.2" : std::string(provider.versionName)=="3.1.6","expected actual provider");
        if (force) { const auto id=Observe(runtime.Upscaler(),generation,"created"); Require(id.found && !id.built,"full INT8 provider/model before lazy construction"); }
        for (unsigned frame=0;frame<warmup+measured;++frame) {
            gpu.Begin(); const float jx=Halton(frame%32+1,2), jy=Halton(frame%32+1,3);
            scene.Render(gpu,iw,ih,ow,oh,frame,jx,jy);
            for (auto* input:inputs) Barrier(gpu.list.Get(),input,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ffxDispatchDescUpscale d{}; d.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE; d.commandList=gpu.list.Get();
            d.color=ffxApiGetResourceDX12(color.Get(),FFX_API_RESOURCE_STATE_COMPUTE_READ); d.motionVectors=ffxApiGetResourceDX12(motion.Get(),FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.depth=ffxApiGetResourceDX12(depth.Get(),FFX_API_RESOURCE_STATE_COMPUTE_READ); d.exposure=ffxApiGetResourceDX12(exposure.Get(),FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.output=ffxApiGetResourceDX12(output.Get(),FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            d.jitterOffset={jx,jy}; d.motionVectorScale={float(iw),float(ih)}; d.renderSize={iw,ih}; d.upscaleSize={ow,oh};
            d.frameTimeDelta=16.6667f; d.preExposure=1; d.reset=frame==0 || (run==L"recreate" && frame==warmup+measured/2);
            d.cameraNear=5; d.cameraFar=100000; d.cameraFovAngleVertical=1.04719755f; d.viewSpaceToMetersFactor=.0142875f;
            if (debug) d.flags=FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
            DispatchTrace::Begin(generation,frame); gpu.list->EndQuery(gpu.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
            const auto result=dispatch(&context,&d.header);
            gpu.list->EndQuery(gpu.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1); DispatchTrace::End();
            gpu.list->ResolveQueryData(gpu.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,gpu.timestamps.Get(),0);
            for (auto* input:inputs) Barrier(gpu.list.Get(),input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // Fence partial SDK recordings even if dispatch reported an error.
            gpu.Submit(); DispatchTrace::Complete(gpu.serial); Ffx(result,"dispatch after completed fence");
            if (force && (frame==0 || d.reset || frame==warmup+measured-1)) {
                const auto id=Observe(runtime.Upscaler(),generation,frame==0?"first_dispatch":d.reset?"reset_dispatch":"last_dispatch");
                Require(id.found && id.built,"INT8 lazy model constructed and retained");
            }
            const auto ms=gpu.Milliseconds(); timings<<generation<<','<<frame<<','<<d.reset<<','<<gpu.serial<<','<<ms<<','<<(frame>=warmup)<<'\n'; timings.flush();
            if (frame==0 || frame%60==0) std::printf("PROGRESS generation=%u frame=%u completed_fence=%llu gpu_ms=%.5f\n",generation,frame,gpu.serial,ms);
            if (run==L"recreate" && d.reset) Capture(gpu,output.Get(),out/("reset-g"+std::to_string(generation)+"-f"+std::to_string(frame)+".png"));
        }
        Capture(gpu,output.Get(),out/("output-g"+std::to_string(generation)+".png"));
        Ffx(destroy(&context,&callbacks),"destroy retired context"); std::printf("RETIRED generation=%u fence=%llu\n",generation,gpu.serial);
    }
    std::printf("DISPATCH_OBSERVATION complete=%d\n",DispatchTrace::Available());
    const bool complete=DispatchTrace::Available(); DispatchTrace::Finish();
    if (debug) DebugMessages(gpu.device.Get());
    std::printf("RESULT %s; watermark and shader-inventory cross-check required before neural execution verdict\n",complete?"completed":"UNVERIFIED observation incomplete");
    return complete?0:30;
}
