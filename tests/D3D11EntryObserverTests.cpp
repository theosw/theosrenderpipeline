#include "FrameGen/D3D11EntryObservers.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string_view>

using Microsoft::WRL::ComPtr;
namespace Observers = TheosRenderPipeline::D3D11EntryObservers;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }
static unsigned dispatches{}, copies{}, replays{};
static bool replay{};
static void OnDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z, Observers::Dispatch original)
{
    ++dispatches;
    if (replay) {
        ++replays;
        // Both a trampoline replay and a nested virtual call must bypass
        // observation, including when hooked runtime entries forward to others.
        original(context, x, y, z);
        context->Dispatch(x, y, z);
        Require(!Observers::Ensure(context, OnDispatch, +[](ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*) {}),
            "installation from inside a callback is rejected");
    }
}
static void OnCopy(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*) { ++copies; }

int main(int argc, char** argv)
{
    const bool hardware = argc == 2 && std::string_view(argv[1]) == "--hardware";
    Require(argc == 1 || hardware, "supported arguments");
    Require(!Observers::Ensure(nullptr, OnDispatch, OnCopy), "null context rejected");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    Check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
        nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context), "device");
    constexpr char hlsl[] = "RWTexture2D<uint> dst:register(u0);[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){dst[p.xy]+=1;}";
    ComPtr<ID3DBlob> code;
    Check(D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr), "shader compilation");
    ComPtr<ID3D11ComputeShader> shader;
    Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "shader");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 8; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_UINT; desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> a, b;
    Check(device->CreateTexture2D(&desc, nullptr, &a), "source");
    Check(device->CreateTexture2D(&desc, nullptr, &b), "destination");
    ComPtr<ID3D11UnorderedAccessView> uav;
    Check(device->CreateUnorderedAccessView(a.Get(), nullptr, &uav), "UAV");
    const UINT zero[4]{}; context->ClearUnorderedAccessViewUint(uav.Get(), zero);
    ComPtr<ID3D11Query> disjoint;
    D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    Check(device->CreateQuery(&query, &disjoint), "query");
    ComPtr<ID3D11Device1> device1; ComPtr<ID3D11DeviceContext1> context1;
    Check(device.As(&device1), "device1"); Check(context.As(&context1), "context1");
    ComPtr<ID3DDeviceContextState> scratch, saved;
    Check(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &scratch), "state");
    Require(Observers::Ensure(context.Get(), OnDispatch, OnCopy), "entry installation");
    Require(Observers::Ensure(context.Get(), OnDispatch, OnCopy) && Observers::DispatchEntries() == 1 &&
        Observers::CopyEntries() == 1, "repeated installation retains the original chain");
    Require(!Observers::Ensure(context.Get(), +[](ID3D11DeviceContext*, UINT, UINT, UINT, Observers::Dispatch) {}, OnCopy),
        "another callback cannot replace an installed observer");
    std::set<void*> dispatchTargets, copyTargets;
    unsigned expected{};
    auto exercise = [&](const char* where) {
        const auto* table = *reinterpret_cast<void***>(context.Get());
        dispatchTargets.insert(table[41]); copyTargets.insert(table[47]);
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        replay = expected % 257 == 0;
        context1->Dispatch(1, 1, 1);
        context->CopyResource(b.Get(), a.Get());
        ++expected;
        if (dispatches != expected || copies != expected) {
            std::fprintf(stderr, "after %s: expected=%u dispatch=%u copy=%u\n", where, expected, dispatches, copies);
            Require(false, "each producer call observed exactly once");
        }
    };
    for (unsigned i = 0; i < 2000; ++i) {
        context->Begin(disjoint.Get()); exercise("query begin");
        context->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
        context->GetData(disjoint.Get(), &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        exercise("query read");
        if (i % 10 == 0) {
            context->Flush(); exercise("flush");
            context1->SwapDeviceContextState(scratch.Get(), &saved);
            context1->SwapDeviceContextState(saved.Get(), nullptr); saved.Reset();
            exercise("state swap");
        }
        context->ClearState(); exercise("clear");
        // Discover another callable entry once, then exercise any nested chain.
        // No repair runs between mutation and the next observed producer call.
        if (i == 999) { Require(Observers::Ensure(context.Get(), OnDispatch, OnCopy), "additional runtime entry"); }
    }
    const auto observedCopies = copies;
    context->ClearState();
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), b.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "readback");
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
        UINT value{};
        std::memcpy(&value, static_cast<const char*>(map.pData) + y * map.RowPitch + x * sizeof(value), sizeof(value));
        Require(value == expected + 2 * replays, "GPU executes each forwarded dispatch and requested replay exactly once");
    }
    context->Unmap(staging.Get(), 0);
    std::printf("%s: dispatch=%u copy=%u replays=%u runtime targets=%zu/%zu; mutation continuity and GPU forwarding passed.\n",
        hardware ? "Hardware" : "WARP", dispatches, observedCopies, replays, dispatchTargets.size(), copyTargets.size());
}
