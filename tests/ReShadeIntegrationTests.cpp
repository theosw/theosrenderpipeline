#include "ReShadeIntegration.h"
#include "ReShadeGameDevice.h"
#include <reshade/reshade_events.hpp>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wincodec.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::ReShadeIntegration;
namespace api = reshade::api;
static api::effect_runtime* owned{};
static unsigned draws{}, automaticDraws{}, automaticRuntimes{};
// Both placements exceed ReShade 6.8's 160x120 minimum runtime size.
static constexpr UINT outputWidth = 512, outputHeight = 256;
static constexpr UINT renderWidth = 256, renderHeight = 128;
static UINT expectedDepthWidth{}, expectedDepthHeight{};
static bool depthExtentMatched{};
// Game-device observers: the device add-ons saw created, runtime lifetimes,
// ordinary Presents of our runtime's back buffer and a ReGrade+ exposure.
static api::device* gameAPI{};
static unsigned runtimeInits{}, runtimeDestroys{}, sourcePresents{};
static bool grading{};
// Overlap probe: while ReShade runs our effects, a second thread's UI call must
// be turned away at once rather than wait on the first.
static bool probeOverlap{};
static ID3D11Texture2D* overlapUI{};
static HRESULT overlapResult{S_OK};
static float exposure{};
static void DeviceInit(api::device* device) { if (!gameAPI && device->get_api() == api::device_api::d3d11) { gameAPI = device; } }
static void RuntimeDestroy(api::effect_runtime* runtime) { if (runtime == owned) { ++runtimeDestroys; } }
static void SourcePresent(api::command_queue*, api::swapchain* chain, const api::rect*, const api::rect*, uint32_t, const api::rect*)
{ if (owned && chain && chain->get_current_back_buffer().handle == owned->get_current_back_buffer().handle) { ++sourcePresents; } }
static void Init(api::effect_runtime* runtime)
{ if (runtime->get_device()->get_api() == api::device_api::d3d11) { owned = runtime; ++runtimeInits; } else { ++automaticRuntimes; } }
static void Draw(api::effect_runtime* runtime, api::command_list*, api::resource_view, api::resource_view)
{
    if (runtime != owned) { ++automaticDraws; return; }
    ++draws;
    if (probeOverlap) {
        probeOverlap = false;
        std::thread second([] { overlapResult = ReShadeIntegration::Get().FinishUI(overlapUI); });
        second.join();
    }
    if (grading) {
        // Real ReGrade+ uniforms, set after its own callback has run.
        const auto value = runtime->find_uniform_variable("MartysMods_REGRADE+.fx", "E_EXPOSURE");
        if (!value.handle) { std::fprintf(stderr, "FAIL: ReGrade+ exposure uniform\n"); std::exit(1); }
        runtime->set_uniform_value_float(value, exposure);
        runtime->set_uniform_value_float(runtime->find_uniform_variable("MartysMods_REGRADE+.fx", "E_BYPASS_EXPOSURE"), 0.f);
    }
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
static bool Near(const std::array<unsigned char, 4>& pixel, int r, int g, int b)
{ return std::abs(pixel[0] - r) <= 1 && std::abs(pixel[1] - g) <= 1 && std::abs(pixel[2] - b) <= 1; }
static unsigned PaintedPixels(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "GUI staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "GUI readback");
    unsigned painted{};
    for (UINT y = 0; y < desc.Height; ++y) {
        for (UINT x = 0; x < desc.Width; ++x) {
            const auto* p = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            painted += (p[0] || p[1] || p[2] || p[3]) ? 1 : 0;
        }
    }
    context->Unmap(staging.Get(), 0); return painted;
}
static std::array<unsigned char, 4> PNGPixel(const std::filesystem::path& path)
{
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "PNG factory");
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder), "PNG decoder");
    ComPtr<IWICBitmapFrameDecode> frame; Check(decoder->GetFrame(0, &frame), "PNG frame");
    ComPtr<IWICFormatConverter> converter; Check(factory->CreateFormatConverter(&converter), "PNG converter");
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "PNG RGBA");
    std::array<unsigned char, 4> pixel{}; const WICRect box{0, 0, 1, 1};
    Check(converter->CopyPixels(&box, 4, 4, pixel.data()), "PNG pixel"); return pixel;
}
// IID_UnwrappedObject: the native context under ReShade's wrapper.
static ComPtr<ID3D11DeviceContext> NativeContext(ID3D11DeviceContext* context)
{
    constexpr GUID unwrapped{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
    ComPtr<ID3D11DeviceContext> native;
    Check(context->QueryInterface(unwrapped, reinterpret_cast<void**>(native.GetAddressOf())), "native context");
    return native;
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const bool requireReShade = argc == 2 && std::string_view(argv[1]) == "--require-reshade";
    // Run beside ReShade plus a supplied incompatible add-on (ReGrade+), which
    // would crash inside explicit runtime creation.
    const bool requireBlocked = argc == 2 && std::string_view(argv[1]) == "--require-blocked-addon";
    // The same add-on with the bridge prepared first, as at plugin load: the
    // whole production path must then run with ReGrade+ registered.
    const bool requireBridged = argc == 2 && std::string_view(argv[1]) == "--require-bridged-addon";
    // [Compatibility] ReShadeGameDevice: ReShade's own factory creates the
    // runtime on the game's device. With ReGrade+ supplied, it must grade.
    const bool gameDevice = argc == 2 && std::string_view(argv[1]) == "--require-game-device";
    // The route's prerequisites fail (a native context): ReShade stays off.
    const bool gameDeviceFailure = argc == 2 && std::string_view(argv[1]) == "--require-game-device-failure";
    // Run with Disable=1 in ReShade.ini.
    const bool gameDeviceDisabled = argc == 2 && std::string_view(argv[1]) == "--require-game-device-disabled";
    Require(argc == 1 || requireReShade || requireBlocked || requireBridged || gameDevice || gameDeviceFailure || gameDeviceDisabled,
        "supported arguments");
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
    ReShadeIntegration::Get().UseGameDevice(gameDevice || gameDeviceFailure || gameDeviceDisabled);
    // The blocked case stands for a bridge that was never installed.
    if (!requireBlocked) {
        const std::string_view bridge = ReShadeIntegration::Get().PrepareAddonBridge();
        std::printf("bridge=%.*s\n", static_cast<int>(bridge.size()), bridge.data());
        Require(argc == 1 ? bridge == "ReShade not loaded" : bridge != "ReShade not loaded", "bridge sees whether ReShade is loaded");
        Require(!requireBridged || bridge == "ReGrade+ bridge watching add-on registration", "bridge installed before add-ons load");
    }
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"TRPReShadeFixture";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"TRP offline ReShade fixture", WS_POPUP, 0, 0, outputWidth, outputHeight, nullptr, nullptr, wc.hInstance, nullptr);
    Require(window != nullptr, "hidden fixture window");
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    auto module = GetModuleHandleW(L"dxgi.dll");
    const auto reg = reinterpret_cast<void (*)(reshade::addon_event, void*)>(GetProcAddress(module, "ReShadeRegisterEvent"));
    if (gameDevice) {
        // Register as an add-on first, as the plugin does before the game's
        // device exists; ReShade ignores events from unregistered modules.
        ReShadeIntegration::Get().Discover(window);
        Require(reg != nullptr, "actual ReShade exports");
        reg(reshade::addon_event::init_device, reinterpret_cast<void*>(&DeviceInit));
        reg(reshade::addon_event::destroy_effect_runtime, reinterpret_cast<void*>(&RuntimeDestroy));
        reg(reshade::addon_event::present, reinterpret_cast<void*>(&SourcePresent));
    }
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "D3D11 device");
    auto& effects = ReShadeIntegration::Get(); effects.Discover(window); effects.Configure(device.Get(), context.Get(), {outputWidth, outputHeight});
    effects.SetBeforeUpscaling(false);
    Surface color(device.Get(), outputWidth, outputHeight), earlyColor(device.Get(), renderWidth, renderHeight), depth(device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT), ui(device.Get(), outputWidth, outputHeight);
    const std::array<float, 4> scene{0.25f, 0.5f, 0.25f, 1};
    color.Paint(context.Get(), scene); ui.Paint(context.Get(), scene); depth.Paint(context.Get(), {0.5f, 0, 0, 0});
    // The explicit route ignores the scene; on the game's device it is the
    // frame under the UI layer. ReShade's GUI is drawn into the UI layer
    // there, so each frame starts from a fresh one, like the game's.
    auto uiFill = scene;
    const auto finish = [&] {
        if (gameDevice) { ui.Paint(context.Get(), uiFill); }
        return effects.FinishUI(ui.texture.Get(), color.texture.Get(), true);
    };
    // Device creation is required even when the optional injector is absent.
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; Check(device.As(&dxgi), "DXGI device"); Check(dxgi->GetAdapter(&adapter), "adapter");
    Require(effects.CreateSourceDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, nullptr) == E_POINTER, "null device output rejected");
    ComPtr<ID3D12Device> device12; Check(effects.CreateSourceDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, &device12), "D3D12 device");
    Require(device12.Get() != nullptr, "successful creation returns a device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; ComPtr<ID3D12CommandQueue> queue; Check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
    if (argc == 1) {
        Require(effects.Status() == "ReShade not loaded", "absence fixture must not load an injector");
        ComPtr<ID3D12Fence> ready; Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ready)), "absent runtime fence");
        ComPtr<ID3D12Device> fenceDevice; Check(ready->GetDevice(IID_PPV_ARGS(&fenceDevice)), "absent fence device");
        Require(TheosRenderPipeline::D3D11FrameCopy::SameObject(device12.Get(), fenceDevice.Get()), "absent runtime preserves native device identity");
        Check(queue->Signal(ready.Get(), 1), "absent queue signal");
        HANDLE complete = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(complete != nullptr, "absent fence event");
        Check(ready->SetEventOnCompletion(1, complete), "absent fence completion");
        Require(WaitForSingleObject(complete, 30000) == WAIT_OBJECT_0, "absent queue completes work"); CloseHandle(complete);
        Require(effects.Render(color.texture.Get(), depth.texture.Get(), {outputWidth, outputHeight}, {renderWidth, renderHeight}, false) == S_FALSE, "absent stage is inert");
        Require(finish() == S_FALSE, "absent GUI is inert");
        ReShadeIntegration::ScreenshotRequest shot;
        Require(!effects.TakeScreenshotRequest(shot), "absent ReShade queues no screenshot replacement");
        Require(Pixel(device.Get(), context.Get(), color.texture.Get())[0] == 64, "absence leaves pixels unchanged");
        effects.ResetAfterRetirement(); DestroyWindow(window); std::puts("PASS: absent ReShade creates a working native device and leaves effects inert"); return 0;
    }

    Require(reg != nullptr, "actual ReShade exports");
    reg(reshade::addon_event::init_effect_runtime, reinterpret_cast<void*>(&Init));
    reg(reshade::addon_event::reshade_begin_effects, reinterpret_cast<void*>(&Draw));
    if (gameDeviceDisabled) {
        for (unsigned frame = 0; frame < 10; ++frame) {
            Require(effects.Render(color.texture.Get(), depth.texture.Get(), {outputWidth, outputHeight}, {renderWidth, renderHeight}, false) == S_FALSE, "disabled stage is inert");
            Require(finish() == S_FALSE, "disabled GUI is inert");
            effects.PresentCompleted();
        }
        Require(!owned && !effects.GameDeviceActive() && !effects.Snapshot().failures, "disabled ReShade creates no runtime");
        effects.ResetAfterRetirement(); DestroyWindow(window);
        std::puts("PASS: disabled ReShade stays off on the game-device route");
        return 0;
    }
    if (gameDeviceFailure) {
        // A native context cannot install the GUI route or reach ReShade's factory.
        effects.Configure(device.Get(), NativeContext(context.Get()).Get(), {outputWidth, outputHeight});
        Require(finish() == S_FALSE && effects.Blocked() && !effects.GameDeviceActive() && !owned, "failed route creates no runtime");
        const auto failure = effects.Status();
        std::printf("status=%s\n", failure.c_str());
        Require(failure.find("ReShadeGameDevice failed") != std::string::npos, "status names the failed route and the setting");
        Require(effects.Render(color.texture.Get(), depth.texture.Get(), {outputWidth, outputHeight}, {renderWidth, renderHeight}, false) == S_FALSE &&
            Pixel(device.Get(), context.Get(), color.texture.Get())[0] == 64, "failed route leaves pixels unchanged");
        effects.ResetAfterRetirement(); effects.Configure(device.Get(), context.Get(), {outputWidth, outputHeight});
        Require(finish() == S_FALSE && !owned && effects.Status() == failure && !effects.Snapshot().failures, "the route stays off for the session");
        effects.ResetAfterRetirement(); DestroyWindow(window);
        std::puts("PASS: a failed game-device route turns ReShade off and says why");
        return 0;
    }
    if (requireBlocked) {
        const auto blocked = effects.Status();
        std::printf("status=%s\n", blocked.c_str());
        Require(blocked.find("MARTYSMODS ReGrade+@") != std::string::npos, "incompatible add-on reported at device creation");
        for (unsigned frame = 0; frame < 30; ++frame) {
            color.Paint(context.Get(), scene); ui.Paint(context.Get(), scene);
            Require(effects.Render(color.texture.Get(), depth.texture.Get(), {outputWidth, outputHeight}, {renderWidth, renderHeight}, false) == S_FALSE, "blocked stage is inert");
            Require(finish() == S_FALSE, "blocked GUI is inert");
            effects.PresentCompleted();
        }
        Require(!owned && !automaticRuntimes, "no ReShade runtime was created");
        Require(Pixel(device.Get(), context.Get(), color.texture.Get())[0] == 64 && !draws, "blocked stage leaves pixels unchanged");
        Require(effects.Status() == blocked && !effects.Snapshot().failures, "block is stable and is not a failure");
        effects.ResetAfterRetirement(); effects.Configure(device.Get(), context.Get(), {outputWidth, outputHeight});
        Require(finish() == S_FALSE && !owned, "retirement does not retry runtime creation");
        effects.ResetAfterRetirement(); DestroyWindow(window);
        std::puts("PASS: incompatible add-on withholds runtime creation and leaves effects inert");
        return 0;
    }
    if (requireBridged) {
        std::printf("status=%s\n", effects.Status().c_str());
        Require(!effects.Blocked() && effects.Status().find("ReGrade+ device events bridged") != std::string::npos,
            "bridged add-on reported at device creation");
    }

    // The production device factory keeps this D3D12 output chain free of
    // automatic effect/input runtimes. It presents four times per source frame.
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = outputWidth; desc.Height = outputHeight; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
        Check(effects.Render(color.texture.Get(), depth.texture.Get(), {outputWidth, outputHeight}, {renderWidth, renderHeight}, false), "warm-up effect stage");
        Check(finish(), "warm-up runtime update");
        effects.PresentCompleted(); Sleep(10);
    }
    Require(owned && draws, "actual effect compiled and executed");
    {
        probeOverlap = true; overlapUI = ui.texture.Get();
        const auto before = effects.Snapshot().overlaps;
        // ReShade may reload after its first draw; wait for the next one.
        for (unsigned frame = 0; frame < 300 && probeOverlap; ++frame) {
            Check(effects.Render(color.texture.Get(), depth.texture.Get(), {outputWidth, outputHeight}, {renderWidth, renderHeight}, false), "overlap probe effects");
            if (probeOverlap) { Check(finish(), "overlap probe wait"); effects.PresentCompleted(); Sleep(10); }
        }
        std::printf("overlap probe pending=%d draws=%u result=0x%08X overlaps=%llu\n", int(probeOverlap), draws, static_cast<unsigned>(overlapResult), static_cast<unsigned long long>(effects.Snapshot().overlaps - before));
        Require(!probeOverlap && overlapResult == HRESULT_FROM_WIN32(ERROR_BUSY) && effects.Snapshot().overlaps == before + 1,
            "a second thread is turned away while the first is inside ReShade");
        Check(finish(), "UI after the overlap probe"); effects.PresentCompleted();
    }
    owned->open_overlay(false, api::input_source::none);
    // Validate both placements and live switching. The shader adds 1/8 to red
    // and reads our depth into green; a duplicate render adds another 1/8.
    for (bool before : {false, true, false}) {
        effects.SetBeforeUpscaling(before);
        auto& target = before ? earlyColor : color;
        const TheosRenderPipeline::FrameExtent extent = before ? TheosRenderPipeline::FrameExtent{renderWidth, renderHeight} : TheosRenderPipeline::FrameExtent{outputWidth, outputHeight};
        expectedDepthWidth = extent.width; expectedDepthHeight = extent.height;
        bool validated{}; unsigned stableFrames{};
        for (unsigned frame = 0; frame < 300 && !validated; ++frame) {
            target.Paint(context.Get(), scene); ui.Paint(context.Get(), scene);
            const auto initial = draws;
            Require(effects.Render(target.texture.Get(), depth.texture.Get(), extent, {renderWidth, renderHeight}, !before) == S_FALSE, "unselected placement is inert");
            Check(effects.Render(target.texture.Get(), depth.texture.Get(), extent, {renderWidth, renderHeight}, before), "selected placement");
            Require(effects.Render(target.texture.Get(), depth.texture.Get(), extent, {renderWidth, renderHeight}, before) == S_FALSE, "source-frame deduplication");
            Check(finish(), "GUI update");
            Require(finish() == S_FALSE, "GUI update deduplicated");
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
    if (gameDevice) {
        Require(effects.GameDeviceActive(), "game-device route active");
        std::printf("game device=%p runtime device=%p\n", static_cast<void*>(gameAPI), static_cast<void*>(owned->get_device()));
        Require(gameAPI && owned->get_device() == gameAPI, "runtime runs on the device the game created");
        // Add-ons see the whole frame: the scene with the UI layer over it.
        color.Paint(context.Get(), {0.2f, 0.3f, 0.4f, 1}); uiFill = {0.1f, 0.025f, 0, 0.25f};
        const auto presents = sourcePresents;
        Check(finish(), "frame image capture");
        auto* image = reinterpret_cast<ID3D11Texture2D*>(owned->get_current_back_buffer().handle);
        Require(Near(Pixel(device.Get(), context.Get(), image, outputWidth - 2, outputHeight - 2), 64, 64, 77), "back buffer is the scene under the UI");
        Require(sourcePresents == presents + 1, "one ordinary Present per source frame");
        for (unsigned i = 0; i < 4; ++i) { Check(output->Present(0, 0), "output Present after source"); }
        Require(sourcePresents == presents + 1, "output frames send add-ons no Present");
        effects.PresentCompleted();
        // The route redirects only our image on the native context, and never
        // inside ReShade's effect passes.
        const auto native = NativeContext(context.Get());
        ComPtr<ID3D11RenderTargetView> imageView;
        D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Check(device->CreateRenderTargetView(image, &view, &imageView), "image view");
        const auto bound = [&](ID3D11RenderTargetView* set) {
            native->OMSetRenderTargets(1, &set, nullptr);
            ComPtr<ID3D11RenderTargetView> result; native->OMGetRenderTargets(1, &result, nullptr);
            return result.Get();
        };
        // ReShade draws with protection on; bind the way its Present does.
        ComPtr<ID3D11Multithread> protection; Check(native.As(&protection), "native multithread");
        const auto wasProtected = protection->SetMultithreadProtected(TRUE);
        {
            TheosRenderPipeline::ReShadeGameDevice::GUIRoute route{context.Get(), image, ui.rtv.Get()};
            Require(bool(route), "GUI route starts");
            TheosRenderPipeline::ReShadeGameDevice::RouteGUITable();
            Require(route.Routed(), "the table ReShade draws with is patched");
            Require(bound(imageView.Get()) == ui.rtv.Get(), "image bind goes to the UI layer");
            TheosRenderPipeline::ReShadeGameDevice::BeginEffects();
            Require(bound(imageView.Get()) == imageView.Get(), "effect passes keep their target");
            TheosRenderPipeline::ReShadeGameDevice::FinishEffects();
            Require(bound(color.rtv.Get()) == color.rtv.Get(), "other targets are never redirected");
        }
        Require(bound(imageView.Get()) == imageView.Get(), "no redirection after the route ends");
        protection->SetMultithreadProtected(wasProtected);
        native->ClearState();
    }
    PostMessageW(window, WM_KEYDOWN, VK_F8, 1);
    MSG message{};
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    const auto keyBefore = owned->last_key_pressed();
    for (unsigned i = 0; i < 4; ++i) { Check(output->Present(0, 0), "output Present before source input"); }
    std::printf("pending F8 before output=%u after output=%u\n", keyBefore, owned->last_key_pressed());
    Require(keyBefore == VK_F8 && owned->last_key_pressed() == VK_F8, "output frames must not consume source overlay input");
    Check(finish(), "F8 input update"); effects.PresentCompleted();
    Require(!owned->get_effects_state(), "configured F8 toggles effects off");
    PostMessageW(window, WM_KEYUP, VK_F8, (1u << 31) | (1u << 30) | 1);
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    Check(finish(), "key release update"); effects.PresentCompleted();
    auto pressHome = [&] {
        PostMessageW(window, WM_KEYDOWN, VK_HOME, 1);
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
        for (unsigned i = 0; i < 4; ++i) { Check(output->Present(0, 0), "output before Home"); }
        Check(finish(), "Home input update"); effects.PresentCompleted();
        PostMessageW(window, WM_KEYUP, VK_HOME, (1u << 31) | (1u << 30) | 1);
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
        Check(finish(), "Home release update"); effects.PresentCompleted();
    };
    pressHome(); Require(effects.OverlayOpen(), "configured Home opens the owned overlay");
    pressHome(); Require(!effects.OverlayOpen(), "configured Home closes the owned overlay");
    // The owned runtime saves its UI-layer back buffer; the saved path must
    // reach the presenter's queue exactly once for final-frame replacement.
    ReShadeIntegration::ScreenshotRequest shot;
    Require(!effects.TakeScreenshotRequest(shot), "no screenshot request before the key");
    PostMessageW(window, WM_KEYDOWN, VK_F9, 1);
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    for (unsigned i = 0; i < 4; ++i) { Check(output->Present(0, 0), "output before screenshot key"); }
    Check(finish(), "screenshot key update"); effects.PresentCompleted();
    PostMessageW(window, WM_KEYUP, VK_F9, (1u << 31) | (1u << 30) | 1);
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    bool requested{};
    for (unsigned i = 0; i < 1000 && !requested; ++i) {
        Check(finish(), "screenshot save update");
        requested = effects.TakeScreenshotRequest(shot);
        effects.PresentCompleted();
        if (!requested) { Sleep(10); }
    }
    Require(requested, "owned runtime screenshot is queued after ReShade saves it");
    const auto shotPath = std::filesystem::u8path(shot.path);
    std::printf("screenshot request=%s quality=%d\n", shot.path.c_str(), shot.jpegQuality);
    Require(std::filesystem::exists(shotPath) && shotPath.extension() == ".png" && shotPath.stem() == "TRPProbe",
        "request names ReShade's saved file");
    Require(shot.jpegQuality == 90, "unset JPEG quality uses ReShade's default");
    Require(shot.replaceAllowed, "closed-overlay screenshot permits final-frame replacement");
    if (gameDevice) {
        auto* image = reinterpret_cast<ID3D11Texture2D*>(owned->get_current_back_buffer().handle);
        Require(Near(PNGPixel(shotPath), 64, 64, 77) && Near(Pixel(device.Get(), context.Get(), image), 64, 64, 77),
            "ReShade's screenshot holds the whole frame, not the UI layer alone");
    }
    for (unsigned i = 0; i < 20; ++i) { Check(finish(), "post-screenshot update"); effects.PresentCompleted(); Sleep(5); }
    Require(!effects.TakeScreenshotRequest(shot), "one key press queues one replacement");
    std::filesystem::remove(shotPath);
    // A normal ReShade screenshot deliberately excludes its GUI. Its file must
    // survive when our only available final frame includes the open overlay.
    owned->open_overlay(true, api::input_source::none);
    Require(effects.OverlayOpen(), "open overlay for screenshot preservation");
    PostMessageW(window, WM_KEYDOWN, VK_F9, 1);
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    Check(finish(), "open-overlay screenshot key"); effects.PresentCompleted();
    PostMessageW(window, WM_KEYUP, VK_F9, (1u << 31) | (1u << 30) | 1);
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    requested = false;
    for (unsigned i = 0; i < 1000 && !requested; ++i) {
        Check(finish(), "open-overlay screenshot update");
        requested = effects.TakeScreenshotRequest(shot);
        effects.PresentCompleted();
        if (!requested) { Sleep(10); }
    }
    Require(requested && !shot.replaceAllowed, "GUI-inclusive frame must not overwrite a normal screenshot");
    if (gameDevice) {
        Require(Near(PNGPixel(std::filesystem::u8path(shot.path)), 64, 64, 77), "the kept screenshot holds the world without ReShade's GUI");
    }
    Require(std::filesystem::exists(std::filesystem::u8path(shot.path)), "ReShade original still exists for rejected replacement");
    std::filesystem::remove(std::filesystem::u8path(shot.path));
    owned->open_overlay(false, api::input_source::none);
    owned->set_effects_state(true);
    effects.SetBeforeUpscaling(false);
    color.Paint(context.Get(), scene);
    const auto beforeToggle = draws;
    owned->set_effects_state(false);
    Check(effects.Render(color.texture.Get(), depth.texture.Get(), {outputWidth, outputHeight}, {renderWidth, renderHeight}, false), "disabled effects");
    Require(Pixel(device.Get(), context.Get(), color.texture.Get())[0] == 64 && draws == beforeToggle, "effects toggle preserves world pixels");
    Check(finish(), "effects-off GUI update"); effects.PresentCompleted();
    owned->set_effects_state(true);
    color.Paint(context.Get(), scene);
    ID3D11RenderTargetView* sentinel = ui.rtv.Get(); context->OMSetRenderTargets(1, &sentinel, nullptr);
    const D3D11_VIEWPORT viewport{3, 4, 27, 19, 0.125f, 0.75f}; context->RSSetViewports(1, &viewport);
    Check(effects.Render(color.texture.Get(), nullptr, {outputWidth, outputHeight}, {}, false), "menu frame clears depth semantic");
    ComPtr<ID3D11RenderTargetView> restored; context->OMGetRenderTargets(1, &restored, nullptr);
    D3D11_VIEWPORT restoredViewport{}; UINT count = 1; context->RSGetViewports(&count, &restoredViewport);
    Require(restored.Get() == sentinel && restoredViewport.TopLeftX == 3 && restoredViewport.Width == 27 && restoredViewport.MaxDepth == 0.75f, "effect stage restores producer MRT/viewport state");
    const auto menuPixel = Pixel(device.Get(), context.Get(), color.texture.Get());
    std::printf("menu pixel=%u,%u,%u,%u draws=%u\n", menuPixel[0], menuPixel[1], menuPixel[2], menuPixel[3], draws);
    Require(menuPixel[0] >= 95 && menuPixel[0] <= 97 && menuPixel[1] == 0, "menu does not reuse stale world depth");
    Check(finish(), "menu GUI update"); effects.PresentCompleted();
    context->ClearState();
    Require(owned->open_overlay(true, api::input_source::none), "open ReShade overlay");
    Require(effects.OverlayOpen(), "overlay capture opens");
    if (gameDevice) {
        // ReShade's GUI reaches the transparent UI layer in either protection mode.
        ComPtr<ID3D11Multithread> multithread; Check(context.As(&multithread), "multithread control");
        const auto previous = multithread->GetMultithreadProtected();
        for (const BOOL mode : {FALSE, TRUE}) {
            multithread->SetMultithreadProtected(mode);
            uiFill = {0, 0, 0, 0};
            Check(finish(), "GUI on the UI layer");
            const auto painted = PaintedPixels(device.Get(), context.Get(), ui.texture.Get());
            std::printf("GUI protected=%d painted pixels=%u\n", static_cast<int>(mode), painted);
            Require(painted > 100, "ReShade's GUI is drawn into the UI layer");
            effects.PresentCompleted();
        }
        multithread->SetMultithreadProtected(previous);
    }
    Require(owned->open_overlay(false, api::input_source::none), "close ReShade overlay");
    Require(!effects.OverlayOpen(), "overlay capture releases");
    owned->get_command_queue()->wait_idle();
    effects.ResetAfterRetirement();
    Require(!effects.OverlayOpen(), "retirement releases capture");
    effects.Configure(device.Get(), context.Get(), {outputWidth, outputHeight});
    Check(finish(), "runtime recreated after retirement");
    if (requireBridged) {
        Require(effects.Status().find("ReGrade+ device events bridged") != std::string::npos, "recreated runtime is bridged");
        // Each recreation pairs a new device wrapper's init with its destroy.
        for (unsigned cycle = 0; cycle < 5; ++cycle) {
            effects.PresentCompleted(); owned->get_command_queue()->wait_idle(); effects.ResetAfterRetirement();
            effects.Configure(device.Get(), context.Get(), {outputWidth, outputHeight});
            Check(finish(), "bridged runtime recreation cycle");
        }
    }
    if (gameDevice && GetModuleHandleW(L"MartysMods_ReGradePlus.addon64")) {
        api::effect_technique grade{};
        for (unsigned frame = 0; frame < 1000 && !grade.handle; ++frame) {
            effects.PresentCompleted(); Check(finish(), "ReGrade+ load");
            grade = owned->find_technique("MartysMods_REGRADE+.fx", "MartysMods_ReGradePlus");
            if (!grade.handle) { Sleep(10); }
        }
        const auto probe = owned->find_technique("Probe.fx", "Probe");
        Require(grade.handle && probe.handle, "ReGrade+ technique loaded");
        owned->set_technique_state(probe, false); owned->set_technique_state(grade, true);
        grading = true;
        const auto sample = [&](float value) {
            exposure = value;
            for (unsigned frame = 0; frame < 100; ++frame) {
                effects.PresentCompleted(); color.Paint(context.Get(), {0.15f, 0.25f, 0.35f, 1});
                Check(effects.Render(color.texture.Get(), depth.texture.Get(), {outputWidth, outputHeight}, {renderWidth, renderHeight}, false), "ReGrade+ effects");
                Check(finish(), "ReGrade+ frame"); Sleep(10);
            }
            return Pixel(device.Get(), context.Get(), color.texture.Get(), outputWidth / 2, outputHeight / 2);
        };
        const auto neutral = sample(0), raised = sample(1);
        std::printf("ReGrade+ neutral=%u,%u,%u exposure1=%u,%u,%u\n", neutral[0], neutral[1], neutral[2], raised[0], raised[1], raised[2]);
        Require(raised[0] > neutral[0] + 10 && raised[1] > neutral[1] + 10, "ReGrade+ exposure changes the scene");
        grading = false; owned->set_technique_state(grade, false); owned->set_technique_state(probe, true);
    }
    if (gameDevice) {
        // A new output size recreates the frame image and the runtime.
        effects.PresentCompleted();
        Surface world(device.Get(), outputWidth * 2, outputHeight * 2), layer(device.Get(), outputWidth * 2, outputHeight * 2);
        world.Paint(context.Get(), {0.2f, 0.3f, 0.4f, 1}); layer.Paint(context.Get(), {0, 0, 0, 0});
        effects.Configure(device.Get(), context.Get(), {outputWidth * 2, outputHeight * 2});
        Check(effects.FinishUI(layer.texture.Get(), world.texture.Get(), true), "resized frame");
        const auto resized = owned->get_device()->get_resource_desc(owned->get_current_back_buffer());
        Require(resized.texture.width == outputWidth * 2 && resized.texture.height == outputHeight * 2, "resized back buffer");
    }
    owned->get_command_queue()->wait_idle(); effects.ResetAfterRetirement();
    if (gameDevice) {
        std::printf("runtimes created=%u destroyed=%u ordinary presents=%u\n", runtimeInits, runtimeDestroys, sourcePresents);
        Require(runtimeInits == runtimeDestroys && !effects.GameDeviceActive(), "every adopted runtime is destroyed with its chain");
    }
    std::puts("PASS: actual ReShade placement, depth, source/output ownership, overlay and runtime lifecycle");
    // The output chain owns asynchronous D3D12 work; wait before releasing it.
    ComPtr<ID3D12Fence> fence; Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "retirement fence");
    Check(queue->Signal(fence.Get(), 1), "retirement signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Check(fence->SetEventOnCompletion(1, event), "retirement event");
    Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "output retirement"); CloseHandle(event);
    output.Reset(); DestroyWindow(window);
}
