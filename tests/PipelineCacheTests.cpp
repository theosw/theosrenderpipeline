#include "FrameGen/PipelineCache.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

int main()
{
    // Hardware first; WARP when no hardware adapter offers pipeline libraries.
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGIFactory4> factory;
    Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        ComPtr<IDXGIAdapter> warp;
        Require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
        Require(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
    }
    const auto root = std::filesystem::temp_directory_path() / L"TRPPipelineCacheTests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto file = root / L"nested" / L"xefg-test.bin";

    const char* source = "RWBuffer<uint> o : register(u0); [numthreads(1,1,1)] void main(uint3 i : SV_DispatchThreadID) { o[i.x] = i.x; }";
    ComPtr<ID3DBlob> shader, errors;
    Require(SUCCEEDED(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors)), "compile");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC rootDesc{1, &parameter};
    ComPtr<ID3DBlob> serialized;
    Require(SUCCEEDED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)), "root signature");
    ComPtr<ID3D12RootSignature> signature;
    Require(SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&signature))), "root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature.Get();
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};

    {
        PipelineCache memory;
        memory.Open(device.Get(), {});
        Require(memory.Library() != nullptr, "memory-only library");
        memory.SaveIfGrown();
        Require(!std::filesystem::exists(root), "memory-only library writes no file");
    }
    {
        PipelineCache cache;
        cache.Open(device.Get(), file);
        Require(cache.Library() && cache.Status() == "started empty", "missing file starts an empty library");
        ComPtr<ID3D12PipelineState> pso;
        Require(FAILED(cache.Library()->LoadComputePipeline(L"TRP.Test", &desc, IID_PPV_ARGS(&pso))), "empty library has no pipeline");
        Require(SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso))), "compile pipeline");
        Require(SUCCEEDED(cache.Library()->StorePipeline(L"TRP.Test", pso.Get())), "store pipeline");
        cache.SaveIfGrown();
        Require(std::filesystem::is_regular_file(file) && cache.Status().starts_with("saved"), "grown library saved to a new folder");
        const auto size = std::filesystem::file_size(file);
        cache.SaveIfGrown();
        Require(std::filesystem::file_size(file) == size, "unchanged library is not rewritten");
        Require(!std::filesystem::exists(std::filesystem::path(file) += L".tmp"), "temporary file replaced");
    }
    {
        PipelineCache cache;
        cache.Open(device.Get(), file);
        Require(cache.Status().starts_with("loaded"), "saved library loads");
        ComPtr<ID3D12PipelineState> pso;
        Require(SUCCEEDED(cache.Library()->LoadComputePipeline(L"TRP.Test", &desc, IID_PPV_ARGS(&pso))), "stored pipeline reused");
    }
    {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "not a pipeline library";
        PipelineCache cache;
        cache.Open(device.Get(), file);
        Require(cache.Library() && cache.Status().starts_with("ignored saved file") && cache.Status().ends_with("started empty"),
            "damaged file ignored for an empty library");
    }
    Require(PipelineCacheFile("xefg", 0x10DE, 0x2702, LARGE_INTEGER{.QuadPart = 0x0020000F00174F86}, nullptr).filename() ==
        L"xefg-10de2702-32.15.23.20358-none.bin", "file name carries GPU, driver and runtime");
    std::filesystem::remove_all(root, error);
    std::puts("PASS pipeline cache memory, save, reload and damaged-file recovery");
}
