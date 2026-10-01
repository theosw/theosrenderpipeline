// Standalone upstream ReShade contract observer; does not run Skyrim or third-party add-ons.
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <reshade/reshade_events.hpp>
#include "ReShadeSwapChain.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace api = reshade::api;
using Microsoft::WRL::ComPtr;
using RegisterEvent = void (*)(reshade::addon_event, void *);
using CreateRuntime = bool (*)(api::device_api, void *, void *, void *, const char *, api::effect_runtime **);
using DestroyRuntime = void (*)(api::effect_runtime *);
using UpdateRuntime = void (*)(api::effect_runtime *);
static unsigned sequence{}, frame{};
static const char *phase = "setup";
static std::string mode;
static std::map<api::device *, uint64_t> devices;
static std::map<api::command_queue *, uint64_t> queues;
static api::device *initialDevice{};
static api::command_queue *initialQueue{};
struct RuntimeState { unsigned begins{}, finishes{}, overlays{}, presents{}; bool deviceSeen{}, queueSeen{}; };
static std::map<api::effect_runtime *, RuntimeState> runtimeStates;
static api::effect_runtime *automatic{}, *owned{};
static std::vector<api::effect_runtime *> initOrder;

static void Require(bool ok, const char *why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s; phase=%s\n", why, phase); std::exit(1); }
}
static void Check(HRESULT hr, const char *why)
{
    if (FAILED(hr)) { std::fprintf(stderr, "HRESULT=%08lx ", static_cast<unsigned long>(hr)); Require(false, why); }
}
static uintptr_t ComIdentity(uint64_t handle)
{
    if (!handle) return 0;
    ComPtr<IUnknown> identity;
    if (FAILED(reinterpret_cast<IUnknown *>(handle)->QueryInterface(IID_PPV_ARGS(&identity)))) return 0;
    return reinterpret_cast<uintptr_t>(identity.Get());
}
static int ContextProtection(uint64_t handle)
{
    ComPtr<ID3D11Multithread> protection;
    if (!handle || FAILED(reinterpret_cast<IUnknown *>(handle)->QueryInterface(IID_PPV_ARGS(&protection)))) return -1;
    return protection->GetMultithreadProtected() ? 1 : 0;
}
static void LogObject(const char *event, const void *object, api::device *device, uint64_t native)
{
    std::printf("{\"seq\":%u,\"event\":\"%s\",\"phase\":\"%s\",\"frame\":%u,\"thread\":%lu,\"object\":\"%p\",\"device\":\"%p\",\"native\":\"%llx\",\"com_identity\":\"%llx\"}\n",
        ++sequence, event, phase, frame, GetCurrentThreadId(), object, device,
        static_cast<unsigned long long>(native), static_cast<unsigned long long>(ComIdentity(native)));
    std::fflush(stdout);
}
static void LogRuntime(const char *event, api::effect_runtime *runtime)
{
    auto *device = runtime->get_device();
    auto *queue = runtime->get_command_queue();
    const bool deviceSeen = devices.contains(device), queueSeen = queues.contains(queue);
    const auto nativeDevice = device->get_native(), nativeQueue = queue->get_native();
    std::printf("{\"seq\":%u,\"event\":\"%s\",\"phase\":\"%s\",\"frame\":%u,\"thread\":%lu,\"runtime\":\"%p\",\"device\":\"%p\",\"queue\":\"%p\",\"native_device\":\"%llx\",\"native_queue\":\"%llx\",\"device_identity\":\"%llx\",\"queue_identity\":\"%llx\",\"device_seen\":%s,\"queue_seen\":%s,\"effects\":%s,\"context_protected\":%d,\"f7_pressed\":%s,\"f7_down\":%s}\n",
        ++sequence, event, phase, frame, GetCurrentThreadId(), runtime, device, queue,
        static_cast<unsigned long long>(nativeDevice), static_cast<unsigned long long>(nativeQueue),
        static_cast<unsigned long long>(ComIdentity(nativeDevice)), static_cast<unsigned long long>(ComIdentity(nativeQueue)),
        deviceSeen ? "true" : "false", queueSeen ? "true" : "false", runtime->get_effects_state() ? "true" : "false", ContextProtection(nativeQueue),
        runtime->is_key_pressed(VK_F7) ? "true" : "false", runtime->is_key_down(VK_F7) ? "true" : "false");
    std::fflush(stdout);
}
static void InitDevice(api::device *device)
{
    if (device->get_api() != api::device_api::d3d11) return;
    devices.emplace(device, device->get_native()); initialDevice = device;
    LogObject("init_device", device, device, device->get_native());
}
static void DestroyDevice(api::device *device)
{
    LogObject("destroy_device", device, device, device->get_native()); devices.erase(device);
}
static void InitQueue(api::command_queue *queue)
{
    queues.emplace(queue, queue->get_native()); initialQueue = queue;
    LogObject("init_queue", queue, queue->get_device(), queue->get_native());
}
static void DestroyQueue(api::command_queue *queue)
{
    LogObject("destroy_queue", queue, queue->get_device(), queue->get_native()); queues.erase(queue);
}
static void InitList(api::command_list *list) { LogObject("init_list", list, list->get_device(), list->get_native()); }
static void DestroyList(api::command_list *list) { LogObject("destroy_list", list, list->get_device(), list->get_native()); }
static void InitRuntime(api::effect_runtime *runtime)
{
    RuntimeState state;
    state.deviceSeen = devices.contains(runtime->get_device()); state.queueSeen = queues.contains(runtime->get_command_queue());
    runtimeStates.emplace(runtime, state); initOrder.push_back(runtime);
    if (!automatic && phase == std::string("create_automatic")) automatic = runtime;
    LogRuntime("init_runtime", runtime);
}
static void DestroyRuntimeEvent(api::effect_runtime *runtime) { LogRuntime("destroy_runtime", runtime); }
static void Begin(api::effect_runtime *runtime, api::command_list *, api::resource_view, api::resource_view)
{ ++runtimeStates[runtime].begins; LogRuntime("begin_effects", runtime); }
static void Finish(api::effect_runtime *runtime, api::command_list *, api::resource_view, api::resource_view)
{ ++runtimeStates[runtime].finishes; LogRuntime("finish_effects", runtime); }
static void Overlay(api::effect_runtime *runtime) { ++runtimeStates[runtime].overlays; LogRuntime("overlay", runtime); }
static void Present(api::effect_runtime *runtime) { ++runtimeStates[runtime].presents; LogRuntime("runtime_present", runtime); }
static bool OpenOverlay(api::effect_runtime *runtime, bool open, api::input_source)
{ LogRuntime(open ? "open_overlay" : "close_overlay", runtime); return false; }
static void ProxyPresent(api::command_queue *queue, api::swapchain *, const api::rect *, const api::rect *, uint32_t, const api::rect *)
{ LogObject("proxy_present", queue, queue->get_device(), queue->get_native()); }

struct Surface
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    Surface() = default;
    explicit Surface(ID3D11Device *device)
    {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = 256; desc.Height = 128; desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&desc, nullptr, &texture), "texture");
        D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Check(device->CreateRenderTargetView(texture.Get(), &view, &rtv), "rtv");
    }
    void Paint(ID3D11DeviceContext *context)
    { const float value[4] = {0.25f, 0.25f, 0.25f, 1}; context->ClearRenderTargetView(rtv.Get(), value); }
};
static unsigned Pixel(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback");
    // Away from GUI borders and text, while still inside a valid target.
    const auto red = *(static_cast<const unsigned char *>(mapped.pData) + 110 * mapped.RowPitch + 230 * 4);
    context->Unmap(staging.Get(), 0); return red;
}
static void Pump()
{ MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Require(argc == 2, "one mode required"); mode = argv[1];
    Require(mode == "automatic" || mode == "explicit-native" || mode == "explicit-proxy" || mode == "mixed-auto-first" || mode == "mixed-explicit-first", "supported mode");
    const bool mixed = mode.starts_with("mixed");
    auto module = GetModuleHandleW(L"dxgi.dll"); Require(module != nullptr, "loaded dxgi");
    auto addon = reinterpret_cast<bool (*)(HMODULE, uint32_t)>(GetProcAddress(module, "ReShadeRegisterAddon"));
    auto unregisterAddon = reinterpret_cast<void (*)(HMODULE)>(GetProcAddress(module, "ReShadeUnregisterAddon"));
    auto reg = reinterpret_cast<RegisterEvent>(GetProcAddress(module, "ReShadeRegisterEvent"));
    auto unreg = reinterpret_cast<RegisterEvent>(GetProcAddress(module, "ReShadeUnregisterEvent"));
    auto create = reinterpret_cast<CreateRuntime>(GetProcAddress(module, "ReShadeCreateEffectRuntime"));
    auto destroy = reinterpret_cast<DestroyRuntime>(GetProcAddress(module, "ReShadeDestroyEffectRuntime"));
    auto update = reinterpret_cast<UpdateRuntime>(GetProcAddress(module, "ReShadeUpdateAndPresentEffectRuntime"));
    Require(addon && unregisterAddon && reg && unreg && create && destroy && update, "ReShade API exports");
    Require(addon(GetModuleHandleW(nullptr), 14), "API 14 observer registration");
    const std::pair<reshade::addon_event, void *> callbacks[] = {
        {reshade::addon_event::init_device, reinterpret_cast<void *>(&InitDevice)},
        {reshade::addon_event::destroy_device, reinterpret_cast<void *>(&DestroyDevice)},
        {reshade::addon_event::init_command_queue, reinterpret_cast<void *>(&InitQueue)},
        {reshade::addon_event::destroy_command_queue, reinterpret_cast<void *>(&DestroyQueue)},
        {reshade::addon_event::init_command_list, reinterpret_cast<void *>(&InitList)},
        {reshade::addon_event::destroy_command_list, reinterpret_cast<void *>(&DestroyList)},
        {reshade::addon_event::init_effect_runtime, reinterpret_cast<void *>(&InitRuntime)},
        {reshade::addon_event::destroy_effect_runtime, reinterpret_cast<void *>(&DestroyRuntimeEvent)},
        {reshade::addon_event::reshade_begin_effects, reinterpret_cast<void *>(&Begin)},
        {reshade::addon_event::reshade_finish_effects, reinterpret_cast<void *>(&Finish)},
        {reshade::addon_event::reshade_overlay, reinterpret_cast<void *>(&Overlay)},
        {reshade::addon_event::reshade_present, reinterpret_cast<void *>(&Present)},
        {reshade::addon_event::reshade_open_overlay, reinterpret_cast<void *>(&OpenOverlay)},
        {reshade::addon_event::present, reinterpret_cast<void *>(&ProxyPresent)}
    };
    for (auto [event, fn] : callbacks) reg(event, fn);
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"TRPReShadeLifecycle";
    RegisterClassW(&wc); HWND window = CreateWindowW(wc.lpszClassName, L"Hidden ReShade lifecycle fixture", WS_POPUP, 0, 0, 256, 128, nullptr, nullptr, wc.hInstance, nullptr);
    Require(window != nullptr, "hidden window");
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    // ReShade refuses both WARP driver-type and explicit Basic Render Driver
    // devices. Use its unmodified hardware route with a tiny shader workload.
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "hardware device");
    Require(initialDevice && initialQueue, "normal proxy device/queue notifications captured");
    auto *nativeDevice = reinterpret_cast<ID3D11Device *>(initialDevice->get_native());
    auto *nativeContext = reinterpret_cast<ID3D11DeviceContext *>(initialQueue->get_native());
    Surface explicitSurface(nativeDevice), autoSurface;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<TheosRenderPipeline::ReShadeSwapChain> facade;
    auto makeAuto = [&] {
        phase = "create_automatic";
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = 256; desc.Height = 128; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = desc.BufferCount = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        Check(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swapchain), "automatic swapchain");
        Require(automatic != nullptr, "automatic runtime callback");
        Check(swapchain->GetBuffer(0, IID_PPV_ARGS(&autoSurface.texture)), "automatic buffer");
        Check(nativeDevice->CreateRenderTargetView(autoSurface.texture.Get(), nullptr, &autoSurface.rtv), "automatic rtv");
    };
    auto makeExplicit = [&] {
        phase = "create_explicit";
        const bool proxy = mode == "explicit-proxy";
        auto *passedDevice = proxy ? device.Get() : nativeDevice;
        auto *passedContext = proxy ? context.Get() : nativeContext;
        facade.Attach(new TheosRenderPipeline::ReShadeSwapChain(passedDevice, explicitSurface.texture.Get(), window));
        const auto config = std::filesystem::absolute("Explicit.ini").string();
        Require(create(api::device_api::d3d11, passedDevice, passedContext, facade.Get(), config.c_str(), &owned), "exported runtime creation");
        Require(owned != nullptr, "owned runtime");
    };
    if (mode == "automatic") makeAuto();
    else if (mode == "mixed-auto-first") { makeAuto(); makeExplicit(); }
    else if (mode == "mixed-explicit-first") { makeExplicit(); makeAuto(); }
    else makeExplicit();
    Require(runtimeStates.size() == (mixed ? 2u : 1u), "expected runtime count");
    if (automatic) Require(runtimeStates[automatic].deviceSeen && runtimeStates[automatic].queueSeen, "automatic API identities initialized");
    if (owned) Require(!runtimeStates[owned].deviceSeen && !runtimeStates[owned].queueSeen, "explicit API identities have no matching proxy initialization");
    auto *target = owned ? owned : automatic;
    auto &surface = owned ? explicitSurface : autoSurface;
    auto tick = [&](api::effect_runtime *runtime) {
        LogRuntime("host_tick_before", runtime);
        if (runtime == owned) update(runtime);
        else Check(swapchain->Present(0, 0), "automatic Present");
        LogRuntime("host_tick_after", runtime);
    };
    phase = "warmup";
    for (frame = 0; frame < 300; ++frame) {
        if (automatic) { autoSurface.Paint(nativeContext); tick(automatic); }
        if (owned) { explicitSurface.Paint(nativeContext); tick(owned); }
        Pump();
        if (runtimeStates[target].begins && (!mixed || (runtimeStates[automatic].begins && runtimeStates[owned].begins))) break;
        Sleep(10);
    }
    for (auto *runtime : initOrder)
        Require(runtimeStates[runtime].begins != 0, "each runtime shader warmup completed");
    for (auto *runtime : initOrder) runtime->open_overlay(false, api::input_source::none);
    // A per-device/per-queue add-on-state lookup is the same lookup represented
    // by deviceSeen/queueSeen above. We observe its missing state; we do not fake
    // lifecycle notifications or claim to reproduce a third-party add-on crash.
    phase = "manual_effect_then_present"; ++frame;
    const auto before = runtimeStates[target]; surface.Paint(nativeContext);
    LogRuntime("manual_effect_call_before", target);
    target->render_effects(target->get_command_queue()->get_immediate_command_list(), {reinterpret_cast<uint64_t>(surface.rtv.Get())}, {reinterpret_cast<uint64_t>(surface.rtv.Get())});
    LogRuntime("manual_effect_call_after", target);
    const auto manual = runtimeStates[target];
    Require(manual.begins == before.begins + 1 && manual.finishes == before.finishes + 1, "manual effects executed once");
    tick(target);
    Require(runtimeStates[target].begins == manual.begins, "Present did not duplicate manual effects");
    const auto red = Pixel(nativeDevice, nativeContext, surface.texture.Get());
    Require(red >= 94 && red <= 98, "manual plus Present preserved single shader addition");
    std::printf("{\"event\":\"pixel_check\",\"phase\":\"%s\",\"red\":%u}\n", phase, red);
    phase = "effects_off_overlay_on"; ++frame;
    const auto offBefore = runtimeStates[target]; target->set_effects_state(false);
    Require(target->open_overlay(true, api::input_source::none), "overlay permitted with effects disabled");
    surface.Paint(nativeContext); tick(target);
    Require(runtimeStates[target].begins == offBefore.begins, "ordinary effects disabled");
    Require(runtimeStates[target].overlays > offBefore.overlays && runtimeStates[target].presents == offBefore.presents + 1, "GUI and runtime completion continue with effects disabled");
    target->open_overlay(false, api::input_source::none);
    if (mixed) {
        // Synthetic messages target only our hidden fixture HWND. No SendInput,
        // global key state manipulation, foregrounding, or physical input claim.
        phase = "input_before"; ++frame;
        PostMessageW(window, WM_KEYDOWN, VK_F7, 1); Pump();
        for (auto *runtime : initOrder) LogRuntime("input_snapshot", runtime);
        Require(automatic->is_key_pressed(VK_F7) && owned->is_key_pressed(VK_F7), "both runtimes see shared synthetic key");
        phase = "input_after_secondary"; tick(initOrder[1]);
        for (auto *runtime : initOrder) LogRuntime("input_snapshot", runtime);
        const bool secondaryAdvances = !automatic->is_key_pressed(VK_F7);
        Require(automatic->is_key_pressed(VK_F7) == owned->is_key_pressed(VK_F7), "shared input transitions agree");
        // Older ReShade advances input from every runtime; 6.8 uses a primary
        // handler. Measure the distinction rather than imposing a new-version
        // expectation on an older binary. Re-arm for an independent first-runtime test.
        phase = "input_rearm";
        PostMessageW(window, WM_KEYUP, VK_F7, (1u << 31) | (1u << 30) | 1); Pump(); tick(initOrder[0]);
        PostMessageW(window, WM_KEYDOWN, VK_F7, 1); Pump();
        Require(automatic->is_key_pressed(VK_F7) && owned->is_key_pressed(VK_F7), "fresh shared key for primary check");
        phase = "input_after_primary"; tick(initOrder[0]);
        for (auto *runtime : initOrder) LogRuntime("input_snapshot", runtime);
        Require(!automatic->is_key_pressed(VK_F7) && !owned->is_key_pressed(VK_F7), "first initialized runtime advances shared input");
        std::printf("{\"event\":\"input_policy\",\"secondary_advances\":%s,\"first_advances\":true}\n", secondaryAdvances ? "true" : "false");
        PostMessageW(window, WM_KEYUP, VK_F7, (1u << 31) | (1u << 30) | 1); Pump();
    }
    phase = "teardown";
    for (auto *runtime : initOrder) runtime->get_command_queue()->wait_idle();
    if (owned) { destroy(owned); owned = nullptr; }
    facade.Reset(); autoSurface.rtv.Reset(); autoSurface.texture.Reset(); swapchain.Reset();
    explicitSurface.rtv.Reset(); explicitSurface.texture.Reset(); context.Reset(); device.Reset(); factory.Reset();
    Require(devices.empty() && queues.empty(), "normal proxy device/queue destruction paired");
    DestroyWindow(window);
    for (auto [event, fn] : callbacks) unreg(event, fn);
    unregisterAddon(GetModuleHandleW(nullptr));
    std::printf("{\"event\":\"result\",\"mode\":\"%s\",\"pass\":true,\"runtime_count\":%zu,\"warp\":false}\n", mode.c_str(), runtimeStates.size());
    return 0;
}
