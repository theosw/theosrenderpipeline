#include "ReShadeIntegration.h"
#include "ReShadeSwapChain.h"
#include "FrameGen/D3D11ContextIsolation.h"
#include <Psapi.h>
#include <filesystem>
#include <reshade/reshade_events.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
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
        HMODULE module{}, addon{};
        Create create{}; Destroy destroy{}; Update update{};
        HWND window{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> ui, color, depth, displayDepth, emptyDepth;
        ComPtr<ID3D11RenderTargetView> colorRTV, colorSRGB;
        ComPtr<ID3D11ShaderResourceView> depthSRV, displayDepthSRV, emptyDepthSRV;
        ComPtr<ID3D11UnorderedAccessView> displayDepthUAV;
        ComPtr<ID3D11ComputeShader> depthScale;
        ComPtr<IDXGISwapChain> facade;
        D3D11ContextIsolation isolation;
        D3D11FrameCopy::Depth depthCopy;
        FrameExtent output{};
        api::effect_runtime* runtime{};
        std::atomic_bool overlayOpen{};
        static thread_local ComPtr<ID3D12Device>* capturingDevice;
        bool nativeOutput{}, disabled{};
        std::atomic<api::effect_runtime*> overlayRuntime{};
        static thread_local bool internal;
        bool before{}, attempted{}, updated{};
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
        static bool Open(api::effect_runtime* value, bool open, api::input_source)
        {
            auto& s = Data();
            if (value == s.overlayRuntime.load()) { s.overlayOpen = open; }
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
            if (runtime) { destroy(runtime); runtime = nullptr; }
            overlayOpen = false;
            facade.Reset(); ui.Reset(); color.Reset(); depth.Reset(); emptyDepth.Reset();
            colorRTV.Reset(); colorSRGB.Reset(); depthSRV.Reset(); emptyDepthSRV.Reset();
            displayDepth.Reset(); displayDepthSRV.Reset(); displayDepthUAV.Reset(); depthScale.Reset();
            depthCopy = {};
        }
        HRESULT EnsureRuntime(DXGI_FORMAT format)
        {
            if (!module || !nativeOutput || disabled || !device || !context) { return S_FALSE; }
            if (runtime) {
                D3D11_TEXTURE2D_DESC old{}; ui->GetDesc(&old);
                if (old.Width == output.width && old.Height == output.height && old.Format == StorageFormat(format)) { return S_OK; }
                // Only our D3D11 allocations are replaced here. They never
                // become FG tags; TRP snapshots their completed pixels first.
                runtime->get_command_queue()->wait_idle();
                ReleaseRuntime();
            }
            auto hr = EnsureTexture(ui, output, StorageFormat(format), D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
            if (FAILED(hr)) { return hr; }
            facade.Attach(new ReShadeSwapChain(device.Get(), ui.Get(), window));
            if (!create(api::device_api::d3d11, device.Get(), context.Get(), facade.Get(), config.c_str(), &runtime)) {
                return E_FAIL;
            }
            overlayRuntime = runtime;
            status = "ReShade source-frame stage active";
            return S_OK;
        }
    };

    thread_local bool ReShadeIntegration::State::internal{};
    thread_local ComPtr<ID3D12Device>* ReShadeIntegration::State::capturingDevice{};
    ReShadeIntegration& ReShadeIntegration::Get() { static ReShadeIntegration value; return value; }
    ReShadeIntegration::State& ReShadeIntegration::Data()
    {
        // Keep callback/module ownership for process lifetime. GPU resources
        // release only through the host's explicit retirement boundary.
        static auto* state = new State;
        return *state;
    }
    void ReShadeIntegration::Discover(HWND window)
    {
        auto& s = Data(); s.window = window;
        if (s.module) { return; }
        std::vector<HMODULE> modules(256); DWORD bytes{};
        if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) { return; }
        if (bytes > modules.size() * sizeof(HMODULE)) {
            modules.resize(bytes / sizeof(HMODULE));
            if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), bytes, &bytes)) { return; }
        }
        modules.resize(std::min<std::size_t>(modules.size(), bytes / sizeof(HMODULE)));
        for (const auto module : modules) {
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
            const auto getConfig = reinterpret_cast<bool (*)(HMODULE, api::effect_runtime*, const char*, const char*, char*, size_t*)>(GetProcAddress(module, "ReShadeGetConfigValue"));
            char disabled[16]{}; size_t disabledSize = sizeof(disabled);
            if (getConfig && getConfig(s.addon, nullptr, "GENERAL", "Disable", disabled, &disabledSize)) {
                s.disabled = std::strcmp(disabled, "1") == 0 || _stricmp(disabled, "true") == 0;
            }
            registerEvent(Event::init_device, reinterpret_cast<void*>(&State::InitDevice));
            registerEvent(Event::reshade_open_overlay, reinterpret_cast<void*>(&State::Open));
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
        s.capturingDevice = s.module ? &native : nullptr;
        const auto result = D3D12CreateDevice(adapter, minimum, IID_PPV_ARGS(&exposed));
        s.capturingDevice = nullptr;
        if (FAILED(result)) { return result; }
        s.nativeOutput = native != nullptr;
        if (s.module && s.nativeOutput && !s.disabled) {
            s.status = "ReShade native output isolated; waiting for source UI";
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

    HRESULT ReShadeIntegration::FinishUI(ID3D11Texture2D* ui)
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
        s.update(s.runtime);
        s.updated = true; ++s.counts.updates;
        return s.Failed(s.CopyColor(s.ui.Get(), ui, s.output), "ReShade UI output copy failed");
    }
    void ReShadeIntegration::PresentCompleted() { auto& s = Data(); s.attempted = s.updated = false; }
    void ReShadeIntegration::ResetAfterRetirement()
    {
        auto& s = Data(); State::InternalScope internal{s};
        s.ReleaseRuntime(); s.isolation.ResetAfterRetirement(); s.context.Reset(); s.device.Reset();
        s.attempted = s.updated = false;
    }
    bool ReShadeIntegration::Internal() const { return Data().internal; }
    bool ReShadeIntegration::OverlayOpen() const { return Data().overlayOpen; }
    const std::string& ReShadeIntegration::Status() const { return Data().status; }
    ReShadeIntegration::Counters ReShadeIntegration::Snapshot() const
    { return Data().counts; }
}
