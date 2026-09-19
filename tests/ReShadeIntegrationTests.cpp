#include "ReShadeIntegration.h"
#include <reshade/reshade_events.hpp>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::ReShadeIntegration;
namespace api = reshade::api;
static api::effect_runtime* owned{};
static unsigned draws{}, automaticDraws{}, automaticRuntimes{};
static UINT expectedDepthWidth{}, expectedDepthHeight{};
static bool depthExtentMatched{};
static void Init(api::effect_runtime* runtime)
{ if (runtime->get_device()->get_api() == api::device_api::d3d11) { owned = runtime; } else { ++automaticRuntimes; } }
static void Draw(api::effect_runtime* runtime, api::command_list*, api::resource_view, api::resource_view)
{
    if (runtime != owned) { ++automaticDraws; return; }
    ++draws;
    const auto variable = runtime->find_texture_variable("Probe.fx", "Depth");
    api::resource_view view{}; runtime->get_texture_binding(variable, &view, nullptr);
    const auto desc = runtime->get_device()->get_resource_desc(runtime->get_device()->get_resource_from_view(view));
    depthExtentMatched = desc.texture.width == expectedDepthWidth && desc.texture.height == expectedDepthHeight && desc.texture.format == api::format::r32_float;
}
static void Require(bool ok, const char* why)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s; stage=%s\n", why, ReShadeIntegration::Get().Status().c_str()); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }
struct Surface
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    Surface(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = w; desc.Height = h;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        Check(device->CreateTexture2D(&desc, nullptr, &texture), "texture");
        Check(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv), "RTV");
    }
    void Paint(ID3D11DeviceContext* context, const std::array<float, 4>& color)
    { context->ClearRenderTargetView(rtv.Get(), color.data()); }
};
static std::array<unsigned char, 4> Pixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT x = 0, UINT y = 0)
{
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback");
    auto* p = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
    std::array<unsigned char, 4> value{p[0], p[1], p[2], p[3]}; context->Unmap(staging.Get(), 0); return value;
}

int main(int argc, char** argv)
{
    const bool requireReShade = argc == 2 && std::string_view(argv[1]) == "--require-reshade";
    Require(argc == 1 || requireReShade, "supported arguments");
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"TRPReShadeFixture";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"TRP offline ReShade fixture", WS_POPUP, 0, 0, 64, 32, nullptr, nullptr, wc.hInstance, nullptr);
    Require(window != nullptr, "hidden fixture window");
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "D3D11 device");
    auto& effects = ReShadeIntegration::Get(); effects.Discover(window); effects.Configure(device.Get(), context.Get(), {64, 32});
    effects.SetBeforeUpscaling(false);
    Surface color(device.Get(), 64, 32), earlyColor(device.Get(), 32, 16), depth(device.Get(), 32, 16, DXGI_FORMAT_R32_FLOAT), ui(device.Get(), 64, 32);
    const std::array<float, 4> scene{0.25f, 0.5f, 0.25f, 1};
    color.Paint(context.Get(), scene); ui.Paint(context.Get(), scene); depth.Paint(context.Get(), {0.5f, 0, 0, 0});
    if (!requireReShade) {
        Require(effects.Status() == "ReShade not loaded", "absence fixture must not load an injector");
        Require(effects.Render(color.texture.Get(), depth.texture.Get(), {64, 32}, {32, 16}, false) == S_FALSE, "absent stage is inert");
        Require(effects.FinishUI(ui.texture.Get()) == S_FALSE, "absent GUI is inert");
        Require(Pixel(device.Get(), context.Get(), color.texture.Get())[0] == 64, "absence leaves pixels unchanged");
        effects.ResetAfterRetirement(); DestroyWindow(window); std::puts("PASS: absent ReShade is inert"); return 0;
    }

    auto module = GetModuleHandleW(L"dxgi.dll");
    const auto reg = reinterpret_cast<void (*)(reshade::addon_event, void*)>(GetProcAddress(module, "ReShadeRegisterEvent"));
    Require(reg != nullptr, "actual ReShade exports");
    reg(reshade::addon_event::init_effect_runtime, reinterpret_cast<void*>(&Init));
    reg(reshade::addon_event::reshade_begin_effects, reinterpret_cast<void*>(&Draw));

    // The production device factory keeps this D3D12 output chain free of
    // automatic effect/input runtimes. It presents four times per source frame.
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; Check(device.As(&dxgi), "DXGI device"); Check(dxgi->GetAdapter(&adapter), "adapter");
    ComPtr<ID3D12Device> device12; Check(effects.CreateSourceDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, &device12), "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; ComPtr<ID3D12CommandQueue> queue; Check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = 64; desc.Height = 32; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> output; Check(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &output), "output swapchain");

    Require(automaticRuntimes == 0, "native output creates no automatic ReShade runtime");
    ComPtr<ID3D12Device> foreign; ComPtr<ID3D12Fence> foreignFence;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&foreign)), "unrelated D3D12 creation");
    Check(foreign->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreignFence)), "unrelated fence");
    ComPtr<ID3D12Device> foreignNative; Check(foreignFence->GetDevice(IID_PPV_ARGS(&foreignNative)), "unrelated native identity");
    Require(!TheosRenderPipeline::D3D11FrameCopy::SameObject(foreign.Get(), foreignNative.Get()), "unrelated D3D12 device retains ReShade wrapping");
    foreignFence.Reset(); foreignNative.Reset(); foreign.Reset();
    for (unsigned frame = 0; frame < 300 && !draws; ++frame) {
        color.Paint(context.Get(), scene); ui.Paint(context.Get(), scene);
        Check(effects.Render(color.texture.Get(), depth.texture.Get(), {64, 32}, {32, 16}, false), "warm-up effect stage");
        Check(effects.FinishUI(ui.texture.Get()), "warm-up runtime update");
        effects.PresentCompleted(); Sleep(10);
    }
    Require(owned && draws, "actual effect compiled and executed");
    owned->open_overlay(false, api::input_source::none);
    // Validate both placements and live switching. The shader adds 1/8 to red
    // and reads our depth into green; a duplicate render adds another 1/8.
    for (bool before : {false, true, false}) {
        effects.SetBeforeUpscaling(before);
        auto& target = before ? earlyColor : color;
        const TheosRenderPipeline::FrameExtent extent = before ? TheosRenderPipeline::FrameExtent{32, 16} : TheosRenderPipeline::FrameExtent{64, 32};
        expectedDepthWidth = extent.width; expectedDepthHeight = extent.height;
        bool validated{}; unsigned stableFrames{};
        for (unsigned frame = 0; frame < 300 && !validated; ++frame) {
            target.Paint(context.Get(), scene); ui.Paint(context.Get(), scene);
            const auto initial = draws;
            Require(effects.Render(target.texture.Get(), depth.texture.Get(), extent, {32, 16}, !before) == S_FALSE, "unselected placement is inert");
            Check(effects.Render(target.texture.Get(), depth.texture.Get(), extent, {32, 16}, before), "selected placement");
            Require(effects.Render(target.texture.Get(), depth.texture.Get(), extent, {32, 16}, before) == S_FALSE, "source-frame deduplication");
            Check(effects.FinishUI(ui.texture.Get()), "GUI update");
            Require(effects.FinishUI(ui.texture.Get()) == S_FALSE, "GUI update deduplicated");
            if (draws != initial) {
                const auto pixel = Pixel(device.Get(), context.Get(), target.texture.Get(), extent.width - 1, extent.height - 1);
                std::printf("placement=%s pixel=%u,%u,%u,%u draws=%u\n", before ? "before" : "after", pixel[0], pixel[1], pixel[2], pixel[3], draws - initial);
                Require(depthExtentMatched, "DEPTH uses the selected stage extent and R32_FLOAT");
                const bool correct = pixel[0] >= 95 && pixel[0] <= 97 && pixel[1] >= 127 && pixel[1] <= 129 && pixel[2] >= 190 && pixel[2] <= 192;
                stableFrames = correct ? stableFrames + 1 : 0;
                Require(draws - initial == 1, "one effect invocation per source frame");
                validated = stableFrames >= 3;
            } else { stableFrames = 0; }
            const auto outputDraws = automaticDraws;
            for (unsigned i = 0; i < 4; ++i) { Check(output->Present(0, 0), "downstream output Present"); }
            Require(automaticRuntimes == 0, "output remains free of automatic runtimes");
            Require(automaticDraws == outputDraws, "actual output runtime records no effect draws");
            effects.PresentCompleted(); Sleep(10);
        }
        Require(validated, "placement switch completed shader reload");
    }
    PostMessageW(window, WM_KEYDOWN, VK_F8, 1);
    MSG message{};
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    const auto keyBefore = owned->last_key_pressed();
    for (unsigned i = 0; i < 4; ++i) { Check(output->Present(0, 0), "output Present before source input"); }
    std::printf("pending F8 before output=%u after output=%u\n", keyBefore, owned->last_key_pressed());
    Require(keyBefore == VK_F8 && owned->last_key_pressed() == VK_F8, "output frames must not consume source overlay input");
    Check(effects.FinishUI(ui.texture.Get()), "F8 input update"); effects.PresentCompleted();
    Require(!owned->get_effects_state(), "configured F8 toggles effects off");
    PostMessageW(window, WM_KEYUP, VK_F8, (1u << 31) | (1u << 30) | 1);
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    Check(effects.FinishUI(ui.texture.Get()), "key release update"); effects.PresentCompleted();
    auto pressHome = [&] {
        PostMessageW(window, WM_KEYDOWN, VK_HOME, 1);
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
        for (unsigned i = 0; i < 4; ++i) { Check(output->Present(0, 0), "output before Home"); }
        Check(effects.FinishUI(ui.texture.Get()), "Home input update"); effects.PresentCompleted();
        PostMessageW(window, WM_KEYUP, VK_HOME, (1u << 31) | (1u << 30) | 1);
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
        Check(effects.FinishUI(ui.texture.Get()), "Home release update"); effects.PresentCompleted();
    };
    pressHome(); Require(effects.OverlayOpen(), "configured Home opens the owned overlay");
    pressHome(); Require(!effects.OverlayOpen(), "configured Home closes the owned overlay");
    owned->set_effects_state(true);
    effects.SetBeforeUpscaling(false);
    color.Paint(context.Get(), scene);
    const auto beforeToggle = draws;
    owned->set_effects_state(false);
    Check(effects.Render(color.texture.Get(), depth.texture.Get(), {64, 32}, {32, 16}, false), "disabled effects");
    Require(Pixel(device.Get(), context.Get(), color.texture.Get())[0] == 64 && draws == beforeToggle, "effects toggle preserves world pixels");
    Check(effects.FinishUI(ui.texture.Get()), "effects-off GUI update"); effects.PresentCompleted();
    owned->set_effects_state(true);
    color.Paint(context.Get(), scene);
    ID3D11RenderTargetView* sentinel = ui.rtv.Get(); context->OMSetRenderTargets(1, &sentinel, nullptr);
    const D3D11_VIEWPORT viewport{3, 4, 27, 19, 0.125f, 0.75f}; context->RSSetViewports(1, &viewport);
    Check(effects.Render(color.texture.Get(), nullptr, {64, 32}, {}, false), "menu frame clears depth semantic");
    ComPtr<ID3D11RenderTargetView> restored; context->OMGetRenderTargets(1, &restored, nullptr);
    D3D11_VIEWPORT restoredViewport{}; UINT count = 1; context->RSGetViewports(&count, &restoredViewport);
    Require(restored.Get() == sentinel && restoredViewport.TopLeftX == 3 && restoredViewport.Width == 27 && restoredViewport.MaxDepth == 0.75f, "effect stage restores producer MRT/viewport state");
    const auto menuPixel = Pixel(device.Get(), context.Get(), color.texture.Get());
    std::printf("menu pixel=%u,%u,%u,%u draws=%u\n", menuPixel[0], menuPixel[1], menuPixel[2], menuPixel[3], draws);
    Require(menuPixel[0] >= 95 && menuPixel[0] <= 97 && menuPixel[1] == 0, "menu does not reuse stale world depth");
    Check(effects.FinishUI(ui.texture.Get()), "menu GUI update"); effects.PresentCompleted();
    context->ClearState();
    Require(owned->open_overlay(true, api::input_source::none), "open ReShade overlay");
    Require(effects.OverlayOpen(), "overlay capture opens");
    Require(owned->open_overlay(false, api::input_source::none), "close ReShade overlay");
    Require(!effects.OverlayOpen(), "overlay capture releases");
    owned->get_command_queue()->wait_idle();
    effects.ResetAfterRetirement();
    Require(!effects.OverlayOpen(), "retirement releases capture");
    effects.Configure(device.Get(), context.Get(), {64, 32});
    Check(effects.FinishUI(ui.texture.Get()), "runtime recreated after retirement");
    owned->get_command_queue()->wait_idle(); effects.ResetAfterRetirement();
    std::puts("PASS: actual ReShade placement, depth, source/output ownership, overlay and runtime lifecycle");
    // The output chain owns asynchronous D3D12 work; wait before releasing it.
    ComPtr<ID3D12Fence> fence; Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "retirement fence");
    Check(queue->Signal(fence.Get(), 1), "retirement signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Check(fence->SetEventOnCompletion(1, event), "retirement event");
    Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "output retirement"); CloseHandle(event);
    output.Reset(); DestroyWindow(window);
}
