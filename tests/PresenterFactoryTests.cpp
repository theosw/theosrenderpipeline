#include "FrameGen/PresenterFactory.h"
#include "FrameGen/PresenterReleaseProbe.h"
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>

using namespace TheosRenderPipeline;
using Microsoft::WRL::ComPtr;
static void Require(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}
static void Check(HRESULT hr, const char* what) { Require(SUCCEEDED(hr), what); }

int main()
{
    ComPtr<ID3D12Debug> debug;
    Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug layer");
    debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> adapter;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue, next;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&next)), "new queue");
    auto window = CreateWindowExW(0, L"STATIC", L"TRP native transport test", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "window");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 64; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferCount = 3;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGIFactory2> factory2;
    Check(factory.As(&factory2), "factory2");
    ComPtr<PresenterFactory> capture;
    capture.Attach(new PresenterFactory(factory2.Get()));
    ComPtr<IDXGISwapChain1> chain;
    Check(capture->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain), "capture actual native");
    Require(capture->Native() == chain.Get(), "captured identity");
    ComPtr<IDXGISwapChain> retained = capture->Native();
    chain.Reset(); capture->ReleaseNative(); capture.Reset();
    for (unsigned i = 0; i < 7; ++i) {
        ComPtr<PresenterFactory> reuse;
        reuse.Attach(new PresenterFactory(factory2.Get(), retained.Get()));
        auto wrong = desc; ++wrong.Width;
        Require(reuse->CreateSwapChainForHwnd(next.Get(), window, &wrong, nullptr, nullptr, &chain) == E_INVALIDARG && !chain,
            "changed layout refuses without requesting a second HWND owner");
        Require(reuse->Native() == retained.Get(), "refusal retains native");
        ComPtr<ID3D12Resource> held;
        Check(retained->GetBuffer(0, IID_PPV_ARGS(&held)), "held native buffer");
        Require(FAILED(reuse->CreateSwapChainForHwnd(next.Get(), window, &desc, nullptr, nullptr, &chain)) && !chain,
            "live buffer prevents unsafe rebind");
        held.Reset();
        Check(reuse->CreateSwapChainForHwnd(next.Get(), window, &desc, nullptr, nullptr, &chain), "retired same native rebound");
        Require(chain.Get() == retained.Get(), "exact identity after rebind");
        chain.Reset(); reuse->ReleaseNative(); reuse.Reset();
        PresenterReleaseProbe probe(retained.Get());
        probe.Release([](UINT, void*, HRESULT hr, ULONG) { Check(hr, "native probe"); },
            [](void*, ULONG n) { Require(n == 1, "one explicit owner; no WARP reference growth"); });
    }
    Check(PresenterFactory::NativeFallbackAfterRetirement(retained.Get(), queue.Get()), "fallback rebind");
    DXGI_SWAP_CHAIN_DESC after{};
    Check(retained->GetDesc(&after), "fallback description");
    Require(after.BufferCount == 2, "fallback restores facade buffer count");
    PresenterReleaseProbe final(retained.Get()); retained.Reset();
    final.Release([](UINT, void*, HRESULT hr, ULONG) { Check(hr, "final buffer probe"); },
        [](void*, ULONG n) { Require(n == 0, "final release zero"); });
    ComPtr<ID3D12InfoQueue> messages;
    Check(device.As(&messages), "debug messages");
    // The held-buffer rejection intentionally emits INVALID_CALL diagnostics.
    // No GPU work is submitted in this fixture; real Presents/queue retirement
    // are covered by the separate production SDK hardware fixture.
    DestroyWindow(window);
    std::puts("PASS native capture, seven rebinds, refusal retention, fallback and final zero (WARP)");
}
