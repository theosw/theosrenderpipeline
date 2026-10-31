#include "ReShadeGameDevice.h"
#include "HookSafety.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace TheosRenderPipeline::ReShadeGameDevice
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        // IID_UnwrappedObject in ReShade 6.8.0's com_utils.hpp: a wrapped DXGI or
        // D3D11 object answers it with the native object it forwards to.
        constexpr GUID Unwrapped{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
        template<class T> bool Unwrap(T* object, ComPtr<T>& native)
        { return object && SUCCEEDED(object->QueryInterface(Unwrapped, reinterpret_cast<void**>(native.ReleaseAndGetAddressOf()))) && native; }

        constexpr std::size_t Capacity = 8;
        std::mutex installMutex;

        // Native IDXGIFactory::CreateSwapChain, slot 10. ReShade's factory calls
        // it to create the real chain, then wraps whatever it returns.
        using CreateSwapChain = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
        std::array<CreateSwapChain, Capacity> factoryOriginals{};
        std::array<std::uintptr_t*, Capacity> factorySlots{};
        HookSafety::SlotRegistry factoryRegistry;
        struct Handoff { IDXGIFactory* factory{}; IDXGISwapChain* surface{}; bool taken{}; };
        thread_local Handoff* handoff{};
        template<std::size_t Index>
        HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
        {
            if (handoff && factory == handoff->factory) {
                // Only Wrap's own synchronous call: keep the surface it seeded and
                // never create a real chain. Anything unexpected fails.
                if (!out || *out != handoff->surface || handoff->taken) { return E_UNEXPECTED; }
                handoff->taken = true;
                return S_OK;
            }
            return factoryOriginals[Index](factory, device, desc, out);
        }
        template<std::size_t... Indices> auto CreateSwapChainHooks(std::index_sequence<Indices...>)
        { return std::array<CreateSwapChain, Capacity>{&CreateSwapChainHook<Indices>...}; }

        // Native ID3D11DeviceContext::OMSetRenderTargets, slot 33.
        using Bind = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
        struct Entry { void* target{}; std::atomic<Bind> original{}; bool enabled{}; };
        std::array<Entry, Capacity> bindEntries;
        thread_local unsigned bindDepth{};
    }

    thread_local GUIRoute* GUIRoute::active_{};
    struct Router
    {
        template<std::size_t Index>
        static void STDMETHODCALLTYPE BindHook(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth)
        {
            const auto original = bindEntries[Index].original.load(std::memory_order_acquire);
            const auto* route = GUIRoute::active_;
            if (route && !route->effects_ && !bindDepth && context == route->context_.Get() && count == 1 && views && views[0]) {
                ComPtr<ID3D11Resource> resource;
                views[0]->GetResource(&resource);
                if (resource.Get() == route->image_) { views = &route->target_; }
            }
            ++bindDepth;
            original(context, count, views, depth);
            --bindDepth;
        }
        template<std::size_t... Indices> static auto Hooks(std::index_sequence<Indices...>)
        { return std::array<Bind, Capacity>{&BindHook<Indices>...}; }
    };

    HRESULT Wrap(ID3D11Device* device, IDXGISwapChain* surface, ComPtr<IDXGISwapChain>& chain)
    {
        if (!device || !surface || chain || handoff) { return E_INVALIDARG; }
        ComPtr<IDXGIFactory> factory, native;
        auto hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) { return hr; }
        // Without ReShade's factory wrapper there is no ordinary construction.
        if (!Unwrap(factory.Get(), native) || native.Get() == factory.Get()) { return E_NOINTERFACE; }
        auto* slot = reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<void***>(native.Get())) + 10;
        {
            std::lock_guard lock(installMutex);
            static const auto hooks = CreateSwapChainHooks(std::make_index_sequence<Capacity>{});
            bool installed{};
            for (std::size_t i = 0; i < Capacity; ++i) {
                if (factorySlots[i] && factorySlots[i] != slot) { continue; }
                installed = factoryRegistry.Install(slot, reinterpret_cast<std::uintptr_t>(hooks[i]), factoryOriginals[i]);
                if (installed) { factorySlots[i] = slot; }
                break;
            }
            if (!installed) { return E_FAIL; }
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(hr = surface->GetDesc(&desc))) { return hr; }
        // A real chain cannot be typeless; ReShade checks the description.
        if (desc.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) { desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; }
        if (desc.BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) { desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; }
        Handoff current{native.Get(), surface};
        handoff = &current;
        // The native call keeps this seeded output; ReShade then replaces it with
        // its wrapper, which takes over this reference.
        surface->AddRef();
        IDXGISwapChain* result = surface;
        hr = factory->CreateSwapChain(device, &desc, &result);
        handoff = nullptr;
        if (result) { chain.Attach(result); }
        if (FAILED(hr) || !current.taken || !chain || chain.Get() == surface) {
            chain.Reset();
            return FAILED(hr) ? hr : E_FAIL;
        }
        return S_OK;
    }

    bool PrepareGUIRoute(ID3D11DeviceContext* context)
    {
        // The route needs ReShade's wrapped immediate context.
        ComPtr<ID3D11DeviceContext> native;
        if (!Unwrap(context, native) || native.Get() == context || native->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return false; }
        std::lock_guard lock(installMutex);
        static const auto initialized = MH_Initialize();
        return initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED;
    }

    void RouteGUITable()
    {
        auto* route = GUIRoute::active_;
        if (!route || route->routed_ || bindDepth) { return; }
        static const auto hooks = Router::Hooks(std::make_index_sequence<Capacity>{});
        // D3D11 keeps one table per protection mode and moves to the new one on
        // the next call after a change, so settle it before reading.
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        route->context_->IAGetPrimitiveTopology(&topology);
        auto* target = (*reinterpret_cast<void***>(route->context_.Get()))[33];
        std::lock_guard lock(installMutex);
        for (std::size_t i = 0; i < Capacity; ++i) {
            auto& entry = bindEntries[i];
            if (entry.target && entry.target != target) { continue; }
            if (!entry.target) {
                void* original{};
                if (MH_CreateHook(target, reinterpret_cast<void*>(hooks[i]), &original) != MH_OK) { return; }
                entry.target = target;
                // Publish before enabling; never destroy live trampolines.
                entry.original.store(reinterpret_cast<Bind>(original), std::memory_order_release);
            }
            if (!entry.enabled) { entry.enabled = MH_EnableHook(target) == MH_OK; }
            route->routed_ = entry.enabled;
            return;
        }
    }

    GUIRoute::GUIRoute(ID3D11DeviceContext* context, ID3D11Resource* image, ID3D11RenderTargetView* target)
    {
        if (active_ || !image || !target || !Unwrap(context, context_) || context_->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return; }
        image_ = image; target_ = target; active_ = this;
    }
    GUIRoute::~GUIRoute() { if (active_ == this) { active_ = nullptr; } }
    void BeginEffects() { if (GUIRoute::active_) { ++GUIRoute::active_->effects_; } }
    void FinishEffects() { if (GUIRoute::active_ && GUIRoute::active_->effects_) { --GUIRoute::active_->effects_; } }

    HRESULT FrameImage::Initialize(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT storage, DXGI_FORMAT typed)
    {
        if (!device || !width || !height || image_) { return E_INVALIDARG; }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.Format = storage;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        auto hr = device->CreateTexture2D(&desc, nullptr, &image_);
        if (SUCCEEDED(hr)) { hr = device->CreateTexture2D(&desc, nullptr, &scene_); }
        if (SUCCEEDED(hr)) { hr = device->CreateTexture2D(&desc, nullptr, &ui_); }
        if (FAILED(hr)) { return hr; }
        D3D11_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format = typed; rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = typed; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
        hr = device->CreateRenderTargetView(image_.Get(), &rtv, &rtv_);
        if (SUCCEEDED(hr)) { hr = device->CreateShaderResourceView(scene_.Get(), &srv, &sceneSRV_); }
        if (SUCCEEDED(hr)) { hr = device->CreateShaderResourceView(ui_.Get(), &srv, &uiSRV_); }
        if (FAILED(hr)) { return hr; }
        // The UI layer is premultiplied: source-over onto the scene.
        constexpr char program[] =
            "Texture2D<float4> scene:register(t0); Texture2D<float4> ui:register(t1);"
            "void VS(uint i:SV_VertexID, out float4 p:SV_Position){p=float4(i==2?3:-1,i==1?3:-1,0,1);}"
            "float4 PS(float4 p:SV_Position):SV_Target{float4 s=scene.Load(int3(p.xy,0)),u=ui.Load(int3(p.xy,0));"
            "return float4(u.rgb+s.rgb*(1-u.a),max(u.a,s.a));}";
        ComPtr<ID3DBlob> code;
        hr = D3DCompile(program, sizeof(program) - 1, "ReShadeFrameImage", nullptr, nullptr, "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr);
        if (SUCCEEDED(hr)) { hr = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs_); }
        if (SUCCEEDED(hr)) {
            hr = D3DCompile(program, sizeof(program) - 1, "ReShadeFrameImage", nullptr, nullptr, "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.ReleaseAndGetAddressOf(), nullptr);
        }
        if (SUCCEEDED(hr)) { hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps_); }
        return hr;
    }

    HRESULT FrameImage::Capture(ID3D11DeviceContext* context, ID3D11Texture2D* scene, ID3D11Texture2D* ui, bool uiLayer)
    {
        if (!context || !scene || !image_ || (uiLayer && !ui)) { return E_INVALIDARG; }
        D3D11_TEXTURE2D_DESC expected{}; image_->GetDesc(&expected);
        // Typed and typeless members of one family copy into each other.
        const auto family = [](DXGI_FORMAT format) {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
            default: return format;
            }
        };
        ComPtr<ID3D11Device> owner;
        context->GetDevice(&owner);
        const auto matches = [&](ID3D11Texture2D* texture) {
            ComPtr<ID3D11Device> device; texture->GetDevice(&device);
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            return device.Get() == owner.Get() && desc.Width == expected.Width && desc.Height == expected.Height &&
                desc.MipLevels == 1 && desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && family(desc.Format) == family(expected.Format);
        };
        if (!matches(image_.Get()) || !matches(scene) || (uiLayer && !matches(ui))) { return E_INVALIDARG; }
        if (!uiLayer) { context->CopyResource(image_.Get(), scene); return S_OK; }
        context->CopyResource(scene_.Get(), scene);
        context->CopyResource(ui_.Get(), ui);
        // Set every stage this draw reads; runtime callbacks may leave state.
        context->OMSetBlendState(nullptr, nullptr, ~0u);
        context->OMSetDepthStencilState(nullptr, 0);
        context->RSSetState(nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(ps_.Get(), nullptr, 0);
        auto* target = rtv_.Get();
        context->OMSetRenderTargets(1, &target, nullptr);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(expected.Width), static_cast<float>(expected.Height), 0, 1};
        context->RSSetViewports(1, &viewport);
        ID3D11ShaderResourceView* inputs[]{sceneSRV_.Get(), uiSRV_.Get()};
        context->PSSetShaderResources(0, 2, inputs);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none[2]{};
        context->PSSetShaderResources(0, 2, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        return S_OK;
    }
}
