#include "ReShadeIntegration.h"
#include "ScreenshotWorker.h"
#include "ReShadeSwapChain.h"
#include "ReShadeGameDevice.h"
#include "FrameGen/D3D11ContextIsolation.h"
#include "HookSafety.h"
#include <Psapi.h>
#include <filesystem>
#include <reshade/reshade_events.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace TheosRenderPipeline
{
    using Microsoft::WRL::ComPtr;
    namespace api = reshade::api;
    using Event = reshade::addon_event;

    struct ReShadeIntegration::State
    {
        using Create = bool (*)(api::device_api, void*, void*, void*, const char*, api::effect_runtime**);
        using Destroy = void (*)(api::effect_runtime*);
        using Update = void (*)(api::effect_runtime*);
        using Register = void (*)(Event, void*);
        using GetConfig = bool (*)(HMODULE, api::effect_runtime*, const char*, const char*, char*, size_t*);
        HMODULE module{}, addon{};
        Create create{}; Destroy destroy{}; Update update{}; GetConfig getConfig{};
        std::mutex screenshotMutex;
        std::deque<std::string> screenshots;
        std::atomic_bool screenshotPending{};
        HWND window{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> ui, color, depth, displayDepth, emptyDepth;
        ComPtr<ID3D11RenderTargetView> colorRTV, colorSRGB;
        ComPtr<ID3D11ShaderResourceView> depthSRV, displayDepthSRV, emptyDepthSRV;
        ComPtr<ID3D11UnorderedAccessView> displayDepthUAV;
        ComPtr<ID3D11ComputeShader> depthScale;
        ComPtr<IDXGISwapChain> facade;
        // [Compatibility] ReShadeGameDevice: ReShade's own factory wraps the
        // facade, and the chain it returns owns the runtime (ReShadeGameDevice.h).
        ComPtr<IDXGISwapChain> chain;
        ReShadeGameDevice::FrameImage frame;
        ComPtr<ID3D11RenderTargetView> uiRTV;
        bool gameDevice{}, adopted{}, presented{};
        static thread_local bool wrapping;
        D3D11ContextIsolation isolation;
        D3D11FrameCopy::Depth depthCopy;
        FrameExtent output{};
        api::effect_runtime* runtime{};
        std::atomic_bool overlayOpen{};
        static thread_local ComPtr<ID3D12Device>* capturingDevice;
        bool nativeOutput{}, disabled{}, blocked{}, bridged{};
        // Per thread: another thread's runtime creation is not TRP's own.
        static thread_local bool creating;
        std::atomic<api::effect_runtime*> overlayRuntime{};
        static thread_local bool internal;
        bool before{}, attempted{}, updated{}, screenshotOverlay{};
        Counters counts{};
        std::string config, status{"ReShade not loaded"};

        struct InternalScope
        {
            State& state;
            explicit InternalScope(State& s) : state(s) { state.internal = true; }
            ~InternalScope() { state.internal = false; }
        };

        static void InitDevice(api::device* device)
        {
            if (capturingDevice && device->get_api() == api::device_api::d3d12 && device->get_native()) {
                // Public API handle, not a private proxy GUID or object offset.
                auto* native = reinterpret_cast<ID3D12Device*>(device->get_native());
                native->QueryInterface(IID_PPV_ARGS(capturingDevice->ReleaseAndGetAddressOf()));
            }
        }
        // Game-device route only. ReShade creates its runtime inside Wrap; it is
        // ours if its back buffer is our frame image.
        static void AdoptRuntime(api::effect_runtime* value)
        {
            auto& s = Data();
            if (wrapping && !s.runtime && value->get_device()->get_api() == api::device_api::d3d11 &&
                value->get_current_back_buffer().handle == reinterpret_cast<uint64_t>(s.frame.Texture())) { s.runtime = value; }
        }
        static void ForgetRuntime(api::effect_runtime* value)
        {
            auto& s = Data();
            if (s.adopted && value == s.runtime) { s.overlayRuntime = nullptr; s.overlayOpen = false; s.runtime = nullptr; }
        }
        // After ReShade's GUI, at the end of its Present.
        static void Presented(api::effect_runtime* value)
        { auto& s = Data(); if (s.adopted && value == s.runtime) { s.presented = true; } }
        static void BeginEffects(api::effect_runtime*, api::command_list*, api::resource_view, api::resource_view)
        { ReShadeGameDevice::BeginEffects(); }
        static void FinishEffects(api::effect_runtime*, api::command_list*, api::resource_view, api::resource_view)
        { ReShadeGameDevice::FinishEffects(); }
        static bool Open(api::effect_runtime* value, bool open, api::input_source)
        {
            auto& s = Data();
            if (value == s.overlayRuntime.load()) { s.overlayOpen = open; }
            return false;
        }
        // ReShade's worker thread, after the file was written. Automatic
        // runtimes capture their own presented image and are left alone, as
        // are the " original"/" overlay" variants, which describe this
        // runtime's effect and GUI stages rather than the presented frame.
        static void Screenshot(api::effect_runtime* value, const char* path) noexcept
        {
            ScreenshotBoundary([&] {
                auto& s = Data();
                if (!path || !*path || value != s.overlayRuntime.load()) { return S_OK; }
                const auto stem = std::filesystem::u8path(path).stem().u8string();
                const std::string_view name(reinterpret_cast<const char*>(stem.data()), stem.size());
                if (name.ends_with(" original") || name.ends_with(" overlay")) { return S_OK; }
                std::scoped_lock lock(s.screenshotMutex);
                if (s.screenshots.size() >= 4) { return S_OK; } // Bounded; ReShade keeps its file.
                s.screenshots.emplace_back(path);
                s.screenshotPending = true;
                return S_OK;
            });
        }
        static std::vector<HMODULE> LoadedModules()
        {
            std::vector<HMODULE> modules(256); DWORD bytes{};
            if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) { return {}; }
            if (bytes > modules.size() * sizeof(HMODULE)) {
                modules.resize(bytes / sizeof(HMODULE));
                if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), bytes, &bytes)) { return {}; }
            }
            modules.resize(std::min<std::size_t>(modules.size(), bytes / sizeof(HMODULE)));
            return modules;
        }
        // Add-ons export NAME as a const char* variable, which ReShade reads at
        // registration. Unrelated modules may export another NAME symbol.
        static bool AddonNamed(HMODULE module, std::string_view name)
        {
            const auto exported = reinterpret_cast<std::uintptr_t>(GetProcAddress(module, "NAME"));
            std::uintptr_t text{}; char value[64]{};
            return exported && name.size() < sizeof(value) && HookSafety::Read(exported, &text, sizeof(text)) &&
                HookSafety::Read(text, value, name.size() + 1) && std::string_view(value) == name;
        }
        // ReShadeCreateEffectRuntime wraps our device in new API objects that
        // never receive init_device. ReGrade+ creates its per-device context
        // only in that event and dereferences it unconditionally in
        // init/destroy_effect_runtime, so our runtime creation would crash the
        // game. The bridge captures its handlers as it registers them and runs
        // its own init_device/destroy_device around its runtime events, for our
        // runtime only, as ReShade does for devices it creates. Verified with
        // ReGrade+'s API-18 build of 2026-03-13 on ReShade 6.8.0.
        static constexpr std::string_view regrade{"MARTYSMODS ReGrade+"};
        using DeviceEvent = void (*)(api::device*);
        using RuntimeEvent = void (*)(api::effect_runtime*);
        struct Bridge
        {
            using Change = void (*)(Event, void*);
            using ChangeFor = void (*)(HMODULE, Event, void*);
            Change registerEvent{}, unregisterEvent{};
            ChangeFor registerFor{}, unregisterFor{};
            HMODULE reshade{}, owner{};
            std::atomic<void*> initDevice{}, destroyDevice{}, initRuntime{}, destroyRuntime{};
            api::device* seeded{};
            bool Covers(HMODULE host, HMODULE addon) const
            { return host == reshade && addon == owner && initDevice && destroyDevice && initRuntime && destroyRuntime; }
        };
        static inline Bridge bridge;
        static HMODULE BridgedAddon(void* callback)
        {
            HMODULE owner{};
            return callback && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(callback), &owner) && AddonNamed(owner, regrade) ? owner : nullptr;
        }
        // Its runtime events stay registered to ReGrade+ (removed when it
        // unregisters), at its own place in ReShade's event order.
        static void RegisterEvent(Event ev, void* callback)
        {
            if (const auto owner = BridgedAddon(callback)) {
                bridge.owner = owner;
                switch (ev) {
                case Event::init_device: bridge.initDevice = callback; break;
                case Event::destroy_device: bridge.destroyDevice = callback; break;
                case Event::init_effect_runtime:
                    bridge.initRuntime = callback;
                    return bridge.registerFor(owner, ev, reinterpret_cast<void*>(&InitRuntime));
                case Event::destroy_effect_runtime:
                    bridge.destroyRuntime = callback;
                    return bridge.registerFor(owner, ev, reinterpret_cast<void*>(&DestroyRuntime));
                default: break;
                }
            }
            bridge.registerEvent(ev, callback);
        }
        static void UnregisterEvent(Event ev, void* callback)
        {
            const auto take = [&](std::atomic<void*>& slot, Event expected) {
                auto current = callback;
                return callback && ev == expected && slot.compare_exchange_strong(current, nullptr);
            };
            if (take(bridge.initRuntime, Event::init_effect_runtime)) {
                return bridge.unregisterFor(bridge.owner, ev, reinterpret_cast<void*>(&InitRuntime));
            }
            if (take(bridge.destroyRuntime, Event::destroy_effect_runtime)) {
                return bridge.unregisterFor(bridge.owner, ev, reinterpret_cast<void*>(&DestroyRuntime));
            }
            take(bridge.initDevice, Event::init_device); take(bridge.destroyDevice, Event::destroy_device);
            bridge.unregisterEvent(ev, callback);
        }
        static void InitRuntime(api::effect_runtime* runtime)
        {
            const auto init = reinterpret_cast<DeviceEvent>(bridge.initDevice.load());
            if (Data().creating && init) { bridge.seeded = runtime->get_device(); init(bridge.seeded); }
            if (const auto next = reinterpret_cast<RuntimeEvent>(bridge.initRuntime.load())) { next(runtime); }
        }
        static void DestroyRuntime(api::effect_runtime* runtime)
        {
            if (const auto next = reinterpret_cast<RuntimeEvent>(bridge.destroyRuntime.load())) { next(runtime); }
            // ReShadeDestroyEffectRuntime deletes this device wrapper next.
            if (bridge.seeded && bridge.seeded == runtime->get_device()) {
                if (const auto destroy = reinterpret_cast<DeviceEvent>(bridge.destroyDevice.load())) { destroy(bridge.seeded); }
                bridge.seeded = nullptr;
            }
        }
        // ReShade's add-on search: [ADDON] AddonPath relative to its base path.
        // Only a file named like ReGrade+ installs the bridge; a renamed copy
        // is still caught by the block below.
        static bool RegradeFilePresent(HMODULE module)
        {
            const auto basePath = reinterpret_cast<void (*)(char*, size_t*)>(GetProcAddress(module, "ReShadeGetBasePath"));
            const auto getConfig = reinterpret_cast<GetConfig>(GetProcAddress(module, "ReShadeGetConfigValue"));
            if (!basePath) { return false; }
            try {
                size_t length{}; basePath(nullptr, &length);
                std::string base(length, '\0'); basePath(base.data(), &length); base.resize(std::strlen(base.c_str()));
                auto directory = std::filesystem::u8path(base);
                char configured[MAX_PATH]{}; size_t size = sizeof(configured);
                if (getConfig && getConfig(nullptr, nullptr, "ADDON", "AddonPath", configured, &size) && *configured) {
                    directory /= std::filesystem::u8path(configured);
                }
                std::error_code error;
                for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
                    auto name = entry.path().filename().wstring();
                    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    if (entry.path().extension() == L".addon64" && name.find(L"regrade") != std::wstring::npos) { return true; }
                }
            } catch (...) {}
            return false;
        }
        // Export entries are 32-bit image offsets, so the stubs that receive
        // ReShade's redirected exports must be allocated above the image.
        static std::uint8_t* AllocateAbove(HMODULE module)
        {
            MODULEINFO image{};
            if (!K32GetModuleInformation(GetCurrentProcess(), module, &image, sizeof(image))) { return nullptr; }
            SYSTEM_INFO system{}; GetSystemInfo(&system);
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module), granularity = system.dwAllocationGranularity;
            const auto align = [granularity](std::uintptr_t value) { return (value + granularity - 1) / granularity * granularity; };
            MEMORY_BASIC_INFORMATION region{};
            for (auto address = align(base + image.SizeOfImage); address < base + 0xFFFF0000ull;) {
                if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) { break; }
                if (region.State == MEM_FREE) {
                    if (auto* page = VirtualAlloc(reinterpret_cast<void*>(address), system.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                        return static_cast<std::uint8_t*>(page);
                    }
                }
                address = align(reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize);
            }
            return nullptr;
        }
        // Add-ons resolve ReShade's API with GetProcAddress, so pointing the
        // export entries at our stubs sees every later registration without
        // changing ReShade's code. Lookups made before this keep the original.
        static const char* InstallBridge(HMODULE module)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(module);
            auto* registerSlot = HookSafety::ExportSlot(base, "ReShadeRegisterEvent");
            auto* unregisterSlot = HookSafety::ExportSlot(base, "ReShadeUnregisterEvent");
            const auto registerFor = reinterpret_cast<Bridge::ChangeFor>(GetProcAddress(module, "ReShadeRegisterEventForAddon"));
            const auto unregisterFor = reinterpret_cast<Bridge::ChangeFor>(GetProcAddress(module, "ReShadeUnregisterEventForAddon"));
            if (!registerSlot || !unregisterSlot || !registerFor || !unregisterFor) {
                return "ReGrade+ bridge unavailable: ReShade lacks per-add-on event registration";
            }
            auto* stubs = AllocateAbove(module);
            if (!stubs) { return "ReGrade+ bridge unavailable: no stub memory near ReShade"; }
            bridge.registerEvent = reinterpret_cast<Bridge::Change>(base + *registerSlot);
            bridge.unregisterEvent = reinterpret_cast<Bridge::Change>(base + *unregisterSlot);
            bridge.registerFor = registerFor; bridge.unregisterFor = unregisterFor;
            const auto write = [](std::uint8_t* stub, void* target) {
                constexpr std::uint8_t jump[]{0xFF, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
                std::memcpy(stub, jump, sizeof(jump)); std::memcpy(stub + sizeof(jump), &target, sizeof(target));
            };
            write(stubs, reinterpret_cast<void*>(&RegisterEvent));
            write(stubs + 16, reinterpret_cast<void*>(&UnregisterEvent));
            DWORD previous{};
            if (!VirtualProtect(stubs, 32, PAGE_EXECUTE_READ, &previous)) { VirtualFree(stubs, 0, MEM_RELEASE); return "ReGrade+ bridge unavailable: stub protection failed"; }
            FlushInstructionCache(GetCurrentProcess(), stubs, 32);
            const auto redirect = [&](DWORD* slot, std::uint8_t* stub) {
                DWORD protection{};
                if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) { return false; }
                InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<std::uintptr_t>(stub) - base));
                VirtualProtect(slot, sizeof(*slot), protection, &protection);
                return true;
            };
            // Unregistration first: a captured callback is always released.
            if (!redirect(unregisterSlot, stubs + 16) || !redirect(registerSlot, stubs)) { return "ReGrade+ bridge unavailable: export table is read-only"; }
            bridge.reshade = module;
            return "ReGrade+ bridge watching add-on registration";
        }
        bool BlockIncompatibleAddon()
        {
            // On the game's device ReGrade+ receives its device events as usual.
            if (gameDevice) { return blocked; }
            if (blocked) { return true; }
            for (const auto candidate : LoadedModules()) {
                if (!AddonNamed(candidate, regrade)) { continue; }
                if (bridge.Covers(module, candidate)) { bridged = true; return false; }
                std::wstring path(32768, L'\0');
                path.resize(GetModuleFileNameW(candidate, path.data(), static_cast<DWORD>(path.size())));
                const auto file = std::filesystem::path(path).filename().u8string();
                const auto entry = std::string(regrade) + "@" + std::string(reinterpret_cast<const char*>(file.data()), file.size());
                status = "ReShade effects off: add-on " + std::string(regrade) + " is incompatible with TRP and would crash. "
                    "To use ReShade, add " + entry + " to [ADDON] DisabledAddons in ReShade.ini";
                blocked = true;
                return true;
            }
            return false;
        }
        HRESULT Failed(HRESULT hr, const char* operation)
        {
            if (FAILED(hr)) { ++counts.failures; status = operation; }
            return hr;
        }
        static DXGI_FORMAT StorageFormat(DXGI_FORMAT format)
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
            default: return format;
            }
        }
        HRESULT CopyColor(ID3D11Texture2D* source, ID3D11Texture2D* destination, FrameExtent extent)
        {
            if (!D3D11FrameCopy::ValidResources(context.Get(), source, destination)) { return E_INVALIDARG; }
            D3D11_TEXTURE2D_DESC from{}, to{}; source->GetDesc(&from); destination->GetDesc(&to);
            if (!extent.Fits(from) || !extent.Fits(to) || StorageFormat(from.Format) != StorageFormat(to.Format) ||
                (from.BindFlags & D3D11_BIND_DEPTH_STENCIL) || (to.BindFlags & D3D11_BIND_DEPTH_STENCIL)) { return E_INVALIDARG; }
            const D3D11_BOX box{0, 0, 0, extent.width, extent.height, 1};
            context->CopySubresourceRegion(destination, 0, 0, 0, 0, source, 0, &box);
            return S_OK;
        }
        HRESULT EnsureTexture(ComPtr<ID3D11Texture2D>& texture, FrameExtent extent, DXGI_FORMAT format, UINT bind)
        {
            if (texture) {
                D3D11_TEXTURE2D_DESC old{}; texture->GetDesc(&old);
                if (old.Width == extent.width && old.Height == extent.height && old.Format == format) { return S_OK; }
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = extent.width; desc.Height = extent.height; desc.Format = format;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind;
            ComPtr<ID3D11Texture2D> replacement;
            auto hr = device->CreateTexture2D(&desc, nullptr, &replacement);
            if (SUCCEEDED(hr)) { texture = std::move(replacement); }
            return hr;
        }
        HRESULT ScaleDepth(FrameExtent extent)
        {
            const auto previous = displayDepth.Get();
            auto hr = EnsureTexture(displayDepth, extent, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
            if (FAILED(hr)) { return hr; }
            if (previous != displayDepth.Get() || !displayDepthSRV || !displayDepthUAV) {
                displayDepthSRV.Reset(); displayDepthUAV.Reset();
                hr = device->CreateShaderResourceView(displayDepth.Get(), nullptr, &displayDepthSRV);
                if (SUCCEEDED(hr)) { hr = device->CreateUnorderedAccessView(displayDepth.Get(), nullptr, &displayDepthUAV); }
                if (FAILED(hr)) { return hr; }
            }
            if (!depthScale) {
                // Point sampling preserves foreground/background discontinuities;
                // interpolating hardware depth would invent surfaces at edges.
                constexpr char program[] =
                    "Texture2D<float> s:register(t0); RWTexture2D<float> d:register(u0);"
                    "[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){"
                    "uint w,h,sw,sh; d.GetDimensions(w,h); s.GetDimensions(sw,sh);"
                    "if(p.x<w && p.y<h) d[p.xy]=s.Load(int3(min((p.xy*2+1)*uint2(sw,sh)/(uint2(w,h)*2),uint2(sw,sh)-1),0));}";
                ComPtr<ID3DBlob> code;
                hr = D3DCompile(program, sizeof(program)-1, "ReShadeDepthScale", nullptr, nullptr, "main", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr);
                if (SUCCEEDED(hr)) { hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &depthScale); }
                if (FAILED(hr)) { return hr; }
            }
            context->CSSetShader(depthScale.Get(), nullptr, 0);
            context->CSSetShaderResources(0, 1, depthSRV.GetAddressOf());
            context->CSSetUnorderedAccessViews(0, 1, displayDepthUAV.GetAddressOf(), nullptr);
            context->Dispatch((extent.width+7)/8, (extent.height+7)/8, 1);
            ID3D11ShaderResourceView* noSRV{}; ID3D11UnorderedAccessView* noUAV{};
            context->CSSetShaderResources(0, 1, &noSRV); context->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
            context->CSSetShader(nullptr, nullptr, 0);
            return S_OK;
        }
        void ReleaseRuntime()
        {
            overlayRuntime = nullptr;
            // Releasing the chain destroys an adopted runtime (ForgetRuntime).
            if (chain) { chain.Reset(); runtime = nullptr; }
            else if (runtime) { destroy(runtime); runtime = nullptr; }
            adopted = false; frame.Reset(); uiRTV.Reset();
            overlayOpen = false;
            {
                std::scoped_lock lock(screenshotMutex);
                screenshots.clear(); screenshotPending = false;
            }
            facade.Reset(); ui.Reset(); color.Reset(); depth.Reset(); emptyDepth.Reset();
            colorRTV.Reset(); colorSRGB.Reset(); depthSRV.Reset(); emptyDepthSRV.Reset();
            displayDepth.Reset(); displayDepthSRV.Reset(); displayDepthUAV.Reset(); depthScale.Reset();
            depthCopy = {};
        }
        HRESULT EnsureRuntime(DXGI_FORMAT format)
        {
            if (!module || !nativeOutput || disabled || blocked || !device || !context) { return S_FALSE; }
            if (runtime) {
                D3D11_TEXTURE2D_DESC old{}; ui->GetDesc(&old);
                if (old.Width == output.width && old.Height == output.height && old.Format == StorageFormat(format)) { return S_OK; }
                // Only our D3D11 allocations are replaced here. They never
                // become FG tags; TRP snapshots their completed pixels first.
                runtime->get_command_queue()->wait_idle();
                ReleaseRuntime();
            }
            if (BlockIncompatibleAddon()) { return S_FALSE; }
            auto hr = EnsureTexture(ui, output, StorageFormat(format), D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
            if (FAILED(hr)) { return hr; }
            if (gameDevice) { return CreateOnGameDevice(format); }
            facade.Attach(new ReShadeSwapChain(device.Get(), ui.Get(), window));
            creating = true;
            const bool created = create(api::device_api::d3d11, device.Get(), context.Get(), facade.Get(), config.c_str(), &runtime);
            creating = false;
            // A failed creation deletes its device wrapper, whether or not it
            // reached the destroy event; never treat it as still seeded.
            if (!created) { bridge.seeded = nullptr; return E_FAIL; }
            overlayRuntime = runtime;
            status = bridged ? "ReShade source-frame stage active; ReGrade+ device events bridged" : "ReShade source-frame stage active";
            return S_OK;
        }
        HRESULT CreateOnGameDevice(DXGI_FORMAT format)
        {
            const auto typed = static_cast<DXGI_FORMAT>(api::format_to_default_typed(static_cast<api::format>(format), 0));
            D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format = typed; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            auto hr = frame.Initialize(device.Get(), output.width, output.height, StorageFormat(format), typed);
            if (SUCCEEDED(hr)) { hr = device->CreateRenderTargetView(ui.Get(), &view, &uiRTV); }
            if (FAILED(hr)) { frame.Reset(); uiRTV.Reset(); return hr; }
            const char* failure{};
            if (!ReShadeGameDevice::InstallGUIRoute(context.Get())) {
                failure = "its GUI route could not be installed";
            } else {
                facade.Attach(new ReShadeSwapChain(device.Get(), frame.Texture(), window, true));
                wrapping = true;
                hr = ReShadeGameDevice::Wrap(device.Get(), facade.Get(), chain);
                wrapping = false;
                if (FAILED(hr) || !runtime) { failure = "ReShade's factory did not create its runtime"; }
            }
            if (failure) {
                // Off for the session rather than a half-working mix of routes.
                chain.Reset(); runtime = nullptr; facade.Reset(); frame.Reset(); uiRTV.Reset();
                blocked = true;
                status = std::string("ReShade effects off: ReShadeGameDevice failed, ") + failure +
                    ". Set ReShadeGameDevice=false in TheosRenderPipeline.ini for TRP's usual ReShade route";
                return S_FALSE;
            }
            adopted = true;
            overlayRuntime = runtime;
            status = "ReShade source-frame stage active on the game's device";
            return S_OK;
        }
        // ReShade's own Present on the game's device: input, GUI, screenshots and
        // the add-on present events, once per source frame. Add-ons see the
        // whole frame; the GUI is routed into the UI layer. The surface under
        // the chain presents nothing.
        HRESULT PresentOnGameDevice(ID3D11Texture2D* scene, bool uiLayer)
        {
            auto hr = frame.Capture(context.Get(), scene, ui.Get(), uiLayer);
            if (FAILED(hr)) { return Failed(hr, "ReShade frame image capture failed"); }
            ReShadeGameDevice::GUIRoute route{context.Get(), frame.Texture(), uiRTV.Get()};
            if (!route) { return Failed(E_FAIL, "ReShade GUI route unavailable"); }
            presented = false;
            hr = chain->Present(0, 0);
            if (FAILED(hr)) { return Failed(hr, "ReShade Present on the game's device failed"); }
            runtime->get_command_queue()->flush_immediate_command_list();
            return presented ? S_OK : Failed(E_FAIL, "ReShade skipped its frame on the game's device");
        }
    };

    thread_local bool ReShadeIntegration::State::internal{};
    thread_local bool ReShadeIntegration::State::creating{};
    thread_local bool ReShadeIntegration::State::wrapping{};
    thread_local ComPtr<ID3D12Device>* ReShadeIntegration::State::capturingDevice{};
    ReShadeIntegration& ReShadeIntegration::Get() { static ReShadeIntegration value; return value; }
    ReShadeIntegration::State& ReShadeIntegration::Data()
    {
        // Keep callback/module ownership for process lifetime. GPU resources
        // release only through the host's explicit retirement boundary.
        static auto* state = new State;
        return *state;
    }
    const char* ReShadeIntegration::PrepareAddonBridge()
    {
        if (State::bridge.reshade) { return "ReGrade+ bridge watching add-on registration"; }
        for (const auto module : State::LoadedModules()) {
            if (!GetProcAddress(module, "ReShadeRegisterAddon") || !GetProcAddress(module, "ReShadeRegisterEvent")) { continue; }
            if (!State::RegradeFilePresent(module)) { return "ReShade loaded; no ReGrade+ add-on file"; }
            return State::InstallBridge(module);
        }
        return "ReShade not loaded";
    }
    void ReShadeIntegration::UseGameDevice(bool enabled) { auto& s = Data(); if (!s.module) { s.gameDevice = enabled; } }
    void ReShadeIntegration::Discover(HWND window)
    {
        auto& s = Data(); s.window = window;
        if (s.module) { return; }
        for (const auto module : State::LoadedModules()) {
            const auto registerAddon = reinterpret_cast<bool (*)(HMODULE, uint32_t)>(GetProcAddress(module, "ReShadeRegisterAddon"));
            const auto registerEvent = reinterpret_cast<State::Register>(GetProcAddress(module, "ReShadeRegisterEvent"));
            const auto create = reinterpret_cast<State::Create>(GetProcAddress(module, "ReShadeCreateEffectRuntime"));
            const auto destroy = reinterpret_cast<State::Destroy>(GetProcAddress(module, "ReShadeDestroyEffectRuntime"));
            const auto update = reinterpret_cast<State::Update>(GetProcAddress(module, "ReShadeUpdateAndPresentEffectRuntime"));
            const auto basePath = reinterpret_cast<void (*)(char*, size_t*)>(GetProcAddress(module, "ReShadeGetBasePath"));
            if (!registerAddon) { continue; }
            if (!registerEvent || !create || !destroy || !update || !basePath) {
                s.status = "ReShade public runtime exports unavailable; keeping automatic effects";
                continue;
            }
            HMODULE retained{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &retained)) { continue; }
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&State::InitDevice), &s.addon);
            if (!registerAddon(s.addon, 14)) { FreeLibrary(retained); s.status = "ReShade rejected add-on API 14"; continue; }
            s.module = retained; s.create = create; s.destroy = destroy; s.update = update;
            size_t length{}; basePath(nullptr, &length);
            std::string base(length, '\0'); basePath(base.data(), &length); base.resize(std::strlen(base.c_str()));
            const auto path = (std::filesystem::u8path(base) / "ReShade.ini").u8string();
            s.config.assign(reinterpret_cast<const char*>(path.data()), path.size());
            const auto getConfig = reinterpret_cast<State::GetConfig>(GetProcAddress(module, "ReShadeGetConfigValue"));
            s.getConfig = getConfig;
            char disabled[16]{}; size_t disabledSize = sizeof(disabled);
            if (getConfig && getConfig(s.addon, nullptr, "GENERAL", "Disable", disabled, &disabledSize)) {
                s.disabled = std::strcmp(disabled, "1") == 0 || _stricmp(disabled, "true") == 0;
            }
            registerEvent(Event::init_device, reinterpret_cast<void*>(&State::InitDevice));
            registerEvent(Event::reshade_open_overlay, reinterpret_cast<void*>(&State::Open));
            registerEvent(Event::reshade_screenshot, reinterpret_cast<void*>(&State::Screenshot));
            if (s.gameDevice) {
                registerEvent(Event::init_effect_runtime, reinterpret_cast<void*>(&State::AdoptRuntime));
                registerEvent(Event::destroy_effect_runtime, reinterpret_cast<void*>(&State::ForgetRuntime));
                registerEvent(Event::reshade_begin_effects, reinterpret_cast<void*>(&State::BeginEffects));
                registerEvent(Event::reshade_finish_effects, reinterpret_cast<void*>(&State::FinishEffects));
                registerEvent(Event::reshade_present, reinterpret_cast<void*>(&State::Presented));
            }
            s.status = s.disabled ? "ReShade disabled in ReShade.ini" : "ReShade API 14 registered; waiting for source color";
            return;
        }
    }
    HRESULT ReShadeIntegration::CreateSourceDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimum, ID3D12Device** out)
    {
        if (!out) { return E_POINTER; }
        *out = nullptr;
        auto& s = Data();
        ComPtr<ID3D12Device> exposed, native;
        // Capture only this synchronous host-owned creation, never another mod's
        // device. All of TRP's D3D12 work and Streamline use the same native
        // device/queues; the game-facing D3D11 interfaces stay untouched.
        // ComPtr overloads operator& for output parameters; capture the object itself.
        s.capturingDevice = s.module ? std::addressof(native) : nullptr;
        const auto result = D3D12CreateDevice(adapter, minimum, IID_PPV_ARGS(&exposed));
        s.capturingDevice = nullptr;
        if (FAILED(result)) { return result; }
        s.nativeOutput = native != nullptr;
        if (s.module && s.nativeOutput && !s.disabled) {
            s.status = s.gameDevice ? "ReShade native output isolated; will run on the game's device; waiting for source UI" :
                "ReShade native output isolated; waiting for source UI";
            // ReShade's device hooks have loaded its add-ons by now, so the
            // startup log reports the block. EnsureRuntime checks again.
            if (!s.BlockIncompatibleAddon() && s.bridged) {
                s.status = "ReShade native output isolated; ReGrade+ device events bridged; waiting for source UI";
            }
        }
        if (s.module && !s.nativeOutput) {
            s.status = "ReShade native output ownership unavailable; keeping automatic effects";
        }
        *out = native ? native.Detach() : exposed.Detach();
        return result;
    }
    void ReShadeIntegration::Configure(ID3D11Device* device, ID3D11DeviceContext* context, FrameExtent output)
    { auto& s = Data(); s.device = device; s.context = context; s.output = output; }
    void ReShadeIntegration::SetBeforeUpscaling(bool before) { Data().before = before; }

    HRESULT ReShadeIntegration::Render(ID3D11Texture2D* color, ID3D11Texture2D* depth,
        FrameExtent colorExtent, FrameExtent depthExtent, bool before)
    {
        auto& s = Data();
        if (!s.runtime || s.attempted || s.updated || before != s.before || !color) { return S_FALSE; }
        State::InternalScope internal{s};
        D3D11ContextIsolation::Scope isolation{s.isolation, s.context.Get()};
        if (!isolation) { return s.Failed(E_FAIL, "ReShade context isolation failed"); }
        D3D11_TEXTURE2D_DESC desc{}; color->GetDesc(&desc);
        if (!colorExtent.Fits(desc)) { return s.Failed(E_INVALIDARG, "ReShade color extent invalid"); }
        // The UI boundary creates the runtime with the actual native UI format.
        // Manual effects may use a different extent/format (e.g. CS producer
        // HDR); changing that must not recreate the native GUI runtime.
        if (!s.runtime->get_effects_state()) {
            s.runtime->render_effects(s.runtime->get_command_queue()->get_immediate_command_list(), {0}, {0});
            s.attempted = true;
            return S_OK;
        }
        const auto previous = s.color.Get();
        auto hr = s.EnsureTexture(s.color, colorExtent, State::StorageFormat(desc.Format), D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        if (FAILED(hr)) { return s.Failed(hr, "ReShade color allocation failed"); }
        if (previous != s.color.Get() || !s.colorRTV || !s.colorSRGB) {
            s.colorRTV.Reset(); s.colorSRGB.Reset();
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            view.Format = static_cast<DXGI_FORMAT>(api::format_to_default_typed(static_cast<api::format>(desc.Format), 0));
            hr = s.device->CreateRenderTargetView(s.color.Get(), &view, &s.colorRTV);
            if (FAILED(hr)) { return s.Failed(hr, "ReShade color view failed"); }
            if (view.Format == DXGI_FORMAT_R8G8B8A8_UNORM) { view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
            else if (view.Format == DXGI_FORMAT_B8G8R8A8_UNORM) { view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
            hr = s.device->CreateRenderTargetView(s.color.Get(), &view, &s.colorSRGB);
            if (FAILED(hr)) { return s.Failed(hr, "ReShade sRGB color view failed"); }
        }
        // Clear the semantic first, including menu/missing-guide frames. Never
        // let a preceding world frame's depth survive as today's DEPTH input.
        s.runtime->update_texture_bindings("DEPTH", {0}, {0});
        if (depth) {
            const auto previousDepth = s.depth.Get();
            hr = s.EnsureTexture(s.depth, depthExtent, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
            if (SUCCEEDED(hr) && (previousDepth != s.depth.Get() || !s.depthSRV)) {
                s.depthSRV.Reset(); hr = s.device->CreateShaderResourceView(s.depth.Get(), nullptr, &s.depthSRV);
            }
            if (SUCCEEDED(hr)) { hr = s.depthCopy.Copy(s.context.Get(), depth, s.depth.Get(), depthExtent); }
            if (FAILED(hr)) { return s.Failed(hr, "ReShade depth publication failed"); }
            auto* selectedDepth = s.depthSRV.Get();
            if (colorExtent.width != depthExtent.width || colorExtent.height != depthExtent.height) {
                hr = s.ScaleDepth(colorExtent);
                if (FAILED(hr)) { return s.Failed(hr, "ReShade display depth scaling failed"); }
                selectedDepth = s.displayDepthSRV.Get();
            }
            const api::resource_view view{reinterpret_cast<uint64_t>(selectedDepth)};
            s.runtime->update_texture_bindings("DEPTH", view, view);
        } else {
            if (!s.emptyDepthSRV) {
                // Skyrim uses reversed depth: zero is the far plane. ReShade's
                // generic empty semantic texture is not a game-depth contract.
                D3D11_TEXTURE2D_DESC zero{}; zero.Width = zero.Height = zero.MipLevels = zero.ArraySize = zero.SampleDesc.Count = 1;
                zero.Format = DXGI_FORMAT_R32_FLOAT; zero.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const float farPlane = 0; const D3D11_SUBRESOURCE_DATA data{&farPlane, sizeof(float), sizeof(float)};
                hr = s.device->CreateTexture2D(&zero, &data, s.emptyDepth.ReleaseAndGetAddressOf());
                if (SUCCEEDED(hr)) { hr = s.device->CreateShaderResourceView(s.emptyDepth.Get(), nullptr, &s.emptyDepthSRV); }
                if (FAILED(hr)) { return s.Failed(hr, "ReShade empty depth allocation failed"); }
            }
            const api::resource_view view{reinterpret_cast<uint64_t>(s.emptyDepthSRV.Get())};
            s.runtime->update_texture_bindings("DEPTH", view, view);
        }
        hr = s.CopyColor(color, s.color.Get(), colorExtent);
        if (FAILED(hr)) { return s.Failed(hr, "ReShade input copy failed"); }
        s.attempted = true;
        s.runtime->render_effects(s.runtime->get_command_queue()->get_immediate_command_list(),
            {reinterpret_cast<uint64_t>(s.colorRTV.Get())}, {reinterpret_cast<uint64_t>(s.colorSRGB.Get())});
        ++s.counts.effects;
        s.status = before ? "ReShade before upscaling" : "ReShade after upscaling";
        // API 14 queues a shader reload when the target extent/format changes,
        // but can still issue the old-size passes in that first call. Discard
        // that transitional result instead of displaying a partially shaded frame.
        if (previous != s.color.Get()) { return S_OK; }
        return s.Failed(s.CopyColor(s.color.Get(), color, colorExtent), "ReShade output copy failed");
    }

    HRESULT ReShadeIntegration::FinishUI(ID3D11Texture2D* ui, ID3D11Texture2D* scene, bool uiLayer)
    {
        auto& s = Data();
        if (!s.module || s.updated || !ui) { return S_FALSE; }
        State::InternalScope internal{s};
        D3D11ContextIsolation::Scope isolation{s.isolation, s.context.Get()};
        if (!isolation) { return s.Failed(E_FAIL, "ReShade UI context unavailable"); }
        D3D11_TEXTURE2D_DESC desc{}; ui->GetDesc(&desc);
        if (desc.Width != s.output.width || desc.Height != s.output.height) { return s.Failed(E_INVALIDARG, "ReShade UI extent mismatch"); }
        auto hr = s.EnsureRuntime(desc.Format);
        if (hr != S_OK) { return s.Failed(hr, "ReShade UI runtime unavailable"); }
        // A failed or absent world boundary must not turn Present into an
        // automatic effect stage over the UI. The public guard is idempotent.
        s.runtime->render_effects(s.runtime->get_command_queue()->get_immediate_command_list(), {0}, {0});
        hr = s.CopyColor(ui, s.ui.Get(), s.output);
        if (FAILED(hr)) { return s.Failed(hr, "ReShade UI input copy failed"); }
        // Opening/closing can occur during the update. Conservatively keep
        // ReShade's normal file if either boundary sees the overlay open.
        s.screenshotOverlay = s.overlayOpen.load();
        if (!s.adopted) { s.update(s.runtime); }
        else if (FAILED(hr = s.PresentOnGameDevice(scene, uiLayer))) { return hr; }
        s.screenshotOverlay = s.screenshotOverlay || s.overlayOpen.load();
        s.updated = true; ++s.counts.updates;
        return s.Failed(s.CopyColor(s.ui.Get(), ui, s.output), "ReShade UI output copy failed");
    }
    void ReShadeIntegration::PresentCompleted() { auto& s = Data(); s.attempted = s.updated = false; }
    bool ReShadeIntegration::TakeScreenshotRequest(ScreenshotRequest& request)
    {
        auto& s = Data();
        if (!s.screenshotPending.load()) { return false; }
        {
            std::scoped_lock lock(s.screenshotMutex);
            if (s.screenshots.empty()) { return false; }
            request.path = std::move(s.screenshots.front());
            s.screenshots.pop_front();
            s.screenshotPending = !s.screenshots.empty();
        }
        request.replaceAllowed = s.updated && !s.screenshotOverlay && !s.overlayOpen.load();
        request.jpegQuality = 90; // ReShade's default.
        char value[16]{}; size_t size = sizeof(value);
        if (s.getConfig && s.getConfig(s.addon, s.runtime, "SCREENSHOT", "JPEGQuality", value, &size)) {
            if (const auto quality = std::atoi(value); quality > 0) { request.jpegQuality = quality; }
        }
        return true;
    }
    void ReShadeIntegration::ResetAfterRetirement()
    {
        auto& s = Data(); State::InternalScope internal{s};
        s.ReleaseRuntime(); s.isolation.ResetAfterRetirement(); s.context.Reset(); s.device.Reset();
        s.attempted = s.updated = false;
    }
    bool ReShadeIntegration::Internal() const { return Data().internal; }
    bool ReShadeIntegration::OverlayOpen() const { return Data().overlayOpen; }
    bool ReShadeIntegration::Blocked() const { return Data().blocked; }
    bool ReShadeIntegration::GameDeviceActive() const { return Data().adopted; }
    const std::string& ReShadeIntegration::Status() const { return Data().status; }
    ReShadeIntegration::Counters ReShadeIntegration::Snapshot() const
    { return Data().counts; }
}
