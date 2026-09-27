#include "FrameGen/D3D11LiveSlot.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::D3D11LiveSlot;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

using Dispatch = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
static D3D11LiveSlot slot;
static unsigned observed{};
static void STDMETHODCALLTYPE Observe(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    ++observed;
    reinterpret_cast<Dispatch>(slot.Original())(context, x, y, z);
}

int main()
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &device, nullptr, &context), "WARP device");
    ComPtr<ID3D11Device1> device1; ComPtr<ID3D11DeviceContext1> context1;
    Check(device.As(&device1), "device1"); Check(context.As(&context1), "context1");
    ComPtr<ID3DDeviceContextState> scratch, saved; D3D_FEATURE_LEVEL selected{};
    Check(device1->CreateDeviceContextState(0, levels, 2, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
        &selected, &scratch), "scratch state");
    const auto hook = reinterpret_cast<std::uintptr_t>(&Observe);

    Require(slot.Ensure(context.Get(), 41, hook) && slot.Installs() == 1, "initial install");
    Require(slot.Ensure(context.Get(), 41, hook) && slot.Installs() == 1, "an installed slot is not rewritten");
    context->Dispatch(0, 0, 0);
    Require(observed == 1, "hooked producer dispatch observed and forwarded");

    // The runtime rewrites the table on a state swap round trip, as our
    // isolation performs every frame. The hook must not be assumed to survive.
    context1->SwapDeviceContextState(scratch.Get(), &saved);
    context1->SwapDeviceContextState(saved.Get(), nullptr);
    context->Dispatch(0, 0, 0);
    Require(observed == 1, "state swap removes context table hooks (runtime behaviour this helper exists for)");

    Require(slot.Ensure(context.Get(), 41, hook) && slot.Installs() == 2, "reinstall after swap");
    context->Dispatch(0, 0, 0);
    Require(observed == 2, "reinstalled hook observes the next producer dispatch");
    Require(!slot.Ensure(nullptr, 41, hook), "null context rejected");
    std::puts("D3D11 live slot: state-swap removal reproduced; reinstall and forwarding passed.");
}
