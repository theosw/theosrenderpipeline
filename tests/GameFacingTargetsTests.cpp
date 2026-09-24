#include "FrameGen/GameFacingTargets.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using Targets = TheosRenderPipeline::GameFacingTargets;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

static D3D11_TEXTURE2D_DESC PresentationDesc(DXGI_FORMAT format, UINT width = 5120, UINT height = 1440)
{
    D3D11_TEXTURE2D_DESC output{};
    output.Width = width;
    output.Height = height;
    output.MipLevels = 1;
    output.ArraySize = 1;
    output.Format = format;
    output.SampleDesc.Count = 1;
    output.Usage = D3D11_USAGE_DEFAULT;
    output.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    output.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    return output;
}

// ---------------------------------------------------------------------------
// Descriptor contracts
// ---------------------------------------------------------------------------

static void DescriptorContracts()
{
    const auto output = PresentationDesc(DXGI_FORMAT_R8G8B8A8_UNORM);
    constexpr UINT renderWidth = 3413;
    constexpr UINT renderHeight = 960;

    const auto gameFacing = Targets::GameFacingDesc(output, renderWidth, renderHeight);
    Require(gameFacing.Width == renderWidth && gameFacing.Height == renderHeight,
        "the game-facing texture carries the render extent");
    Require((gameFacing.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0,
        "the game-facing texture retains its unordered-access binding");
    Require(gameFacing.MiscFlags == output.MiscFlags,
        "the game-facing texture preserves the inner shared-resource contract");

    const auto input = Targets::UpscaleInputDesc(output, renderWidth, renderHeight);
    Require(input.Width == renderWidth && input.Height == renderHeight,
        "the upscale input is allocated at the render extent");
    Require(input.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
        "the upscale input stays a plain SRV/RTV copy destination");
    Require(input.MiscFlags == 0 && input.CPUAccessFlags == 0 && input.Usage == D3D11_USAGE_DEFAULT,
        "the upscale input drops the inner metadata it cannot honor");

    // The proven path: DLSS evaluates into its own UAV and copies here.
    const auto handoff = Targets::UpscaleOutputDesc(output);
    Require(handoff.Width == output.Width && handoff.Height == output.Height,
        "the handoff target is allocated at the output extent");
    Require(handoff.Format == output.Format,
        "the handoff target matches the presentation format for CopyResource");
    Require(handoff.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
        "the default handoff target keeps exactly the proven bindings");

    // The opt-in direct-output routes bind this texture as the NGX or RCAS
    // destination. Without the unordered-access binding every route request is
    // rejected for the session and the per-frame copy can never be removed.
    const auto direct = Targets::UpscaleOutputDesc(output, true);
    Require((direct.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0,
        "the direct-output handoff target is unordered-access capable");
    Require(direct.BindFlags == (handoff.BindFlags | D3D11_BIND_UNORDERED_ACCESS),
        "requesting unordered access only adds that binding");

    // DLSSBackend::EnsureDirectDestinationUAV accepts a destination only when
    // every one of these holds alongside the binding.
    Require(direct.MipLevels == 1 && direct.ArraySize == 1 && direct.SampleDesc.Count == 1 &&
            direct.Usage == D3D11_USAGE_DEFAULT,
        "the direct-output handoff target satisfies the NGX destination contract");
    Require(direct.Width == output.Width && direct.Height == output.Height &&
            direct.Format == handoff.Format,
        "requesting unordered access changes no other allocation input");
}

// ---------------------------------------------------------------------------
// Allocation decision: capability query, attempt order and retry
//
// A format that reports typed unordered-access support can still fail to
// allocate, insufficient video memory being the obvious way. That is a real
// runtime case but not one a real device reproduces on demand, so the decision
// is driven through a double that records every descriptor the production code
// attempts and can be told to reject a specific allocation.
// ---------------------------------------------------------------------------

namespace
{
    struct TextureDouble final : ID3D11Texture2D
    {
        explicit TextureDouble(const D3D11_TEXTURE2D_DESC& desc) : desc_(desc) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override
        {
            if (!object) { return E_POINTER; }
            *object = this;
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto remaining = --references_;
            if (!remaining) { delete this; }
            return remaining;
        }
        void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override { if (device) { *device = nullptr; } }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
        void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* type) override
        {
            if (type) { *type = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
        }
        void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
        UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
        void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* desc) override { if (desc) { *desc = desc_; } }

      private:
        D3D11_TEXTURE2D_DESC desc_{};
        ULONG references_{ 1 };
    };

    // Only CheckFormatSupport and CreateTexture2D carry behavior; the rest of
    // the interface exists so the double can be instantiated at all.
    struct DeviceDouble final : ID3D11Device
    {
        bool reportUnorderedAccess{ false };
        bool rejectUnorderedAccessAllocation{ false };
        bool rejectEveryAllocation{ false };
        std::vector<D3D11_TEXTURE2D_DESC> attempts;

        HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT, UINT* support) override
        {
            if (!support) { return E_POINTER; }
            *support = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
            if (reportUnorderedAccess) { *support |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW; }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc,
            const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D** texture) override
        {
            if (!desc || !texture) { return E_POINTER; }
            attempts.push_back(*desc);
            const bool unorderedAccess = (desc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
            if (rejectEveryAllocation || (unorderedAccess && rejectUnorderedAccessAllocation)) {
                *texture = nullptr;
                return E_OUTOFMEMORY;
            }
            *texture = new TextureDouble(*desc);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override
        {
            if (!object) { return E_POINTER; }
            *object = this;
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }

        HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateVertexShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT, const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreatePixelShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateHullShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateDomainShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateComputeShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC*, ID3D11BlendState**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC*, ID3D11DepthStencilState**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC*, ID3D11RasterizerState**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC*, ID3D11SamplerState**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC*, ID3D11Query**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC*, ID3D11Predicate**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC*, ID3D11Counter**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT, ID3D11DeviceContext**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE, REFIID, void**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT, UINT, UINT*) override { return E_NOTIMPL; }
        void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO*) override {}
        HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC*, D3D11_COUNTER_TYPE*, UINT*, LPSTR, UINT*, LPSTR, UINT*, LPSTR, UINT*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE, void*, UINT) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
        D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return D3D_FEATURE_LEVEL_11_0; }
        UINT STDMETHODCALLTYPE GetCreationFlags() override { return 0; }
        HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }
        void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** context) override { if (context) { *context = nullptr; } }
        HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT) override { return E_NOTIMPL; }
        UINT STDMETHODCALLTYPE GetExceptionMode() override { return 0; }
    };

    bool HasUnorderedAccess(const D3D11_TEXTURE2D_DESC& desc)
    {
        return (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
    }
}

static void AllocationDecision()
{
    const auto output = PresentationDesc(DXGI_FORMAT_R8G8B8A8_UNORM);

    {   // A device without the capability is never asked for the binding.
        Targets targets;
        DeviceDouble device;
        device.reportUnorderedAccess = false;
        Require(SUCCEEDED(targets.CreateUpscaleOutputAfterRetirement(&device, output)),
            "an allocation without unordered-access support still succeeds");
        Require(device.attempts.size() == 1, "an unsupported format costs exactly one allocation");
        Require(!HasUnorderedAccess(device.attempts[0]),
            "an unsupported format is never asked for an unordered-access binding");
        Require(targets.UpscaleOutput() != nullptr, "the proven handoff target is published");
    }

    {   // The capable path: one attempt, and it carries the binding.
        Targets targets;
        DeviceDouble device;
        device.reportUnorderedAccess = true;
        Require(SUCCEEDED(targets.CreateUpscaleOutputAfterRetirement(&device, output)),
            "a capable device allocates the direct-output handoff target");
        Require(device.attempts.size() == 1, "a capable device costs exactly one allocation");
        Require(HasUnorderedAccess(device.attempts[0]),
            "a capable device is asked for the unordered-access binding");
        D3D11_TEXTURE2D_DESC published{};
        targets.UpscaleOutput()->GetDesc(&published);
        Require(HasUnorderedAccess(published), "the published target carries the binding it was allocated with");
    }

    {   // Reported support is not a guarantee that this particular allocation
        // succeeds; memory pressure alone can reject it. The retry, not the
        // capability query, is what keeps that from failing startup.
        Targets targets;
        DeviceDouble device;
        device.reportUnorderedAccess = true;
        device.rejectUnorderedAccessAllocation = true;
        Require(SUCCEEDED(targets.CreateUpscaleOutputAfterRetirement(&device, output)),
            "a rejected unordered-access allocation still produces a handoff target");
        Require(device.attempts.size() == 2, "a rejected allocation is retried exactly once");
        Require(HasUnorderedAccess(device.attempts[0]) && !HasUnorderedAccess(device.attempts[1]),
            "the retry drops the binding instead of repeating the rejected request");
        Require(targets.UpscaleOutput() != nullptr, "the retry publishes the proven handoff target");
        D3D11_TEXTURE2D_DESC published{};
        targets.UpscaleOutput()->GetDesc(&published);
        Require(published.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
            "the retried target is the proven SRV/RTV descriptor");
    }

    {   // A genuine allocation failure is reported, not masked by the retry.
        Targets targets;
        DeviceDouble device;
        device.reportUnorderedAccess = true;
        device.rejectEveryAllocation = true;
        Require(FAILED(targets.CreateUpscaleOutputAfterRetirement(&device, output)),
            "an unallocatable handoff target reports failure");
        Require(device.attempts.size() == 2, "both descriptors are attempted before reporting failure");
        Require(targets.UpscaleOutput() == nullptr,
            "a failed allocation leaves no handoff target behind");
    }

    {   // The retry must not resurrect a previously published target.
        Targets targets;
        DeviceDouble capable;
        capable.reportUnorderedAccess = true;
        Require(SUCCEEDED(targets.CreateUpscaleOutputAfterRetirement(&capable, output)),
            "a first allocation publishes a target");
        DeviceDouble failing;
        failing.reportUnorderedAccess = true;
        failing.rejectEveryAllocation = true;
        Require(FAILED(targets.CreateUpscaleOutputAfterRetirement(&failing, output)),
            "a later failed allocation reports failure");
        Require(targets.UpscaleOutput() == nullptr,
            "a failed reallocation releases the previous target instead of leaving it live");
    }

    {
        Targets targets;
        Require(targets.CreateUpscaleOutputAfterRetirement(nullptr, output) == E_INVALIDARG,
            "a null device is rejected before any allocation");
    }
}

// ---------------------------------------------------------------------------
// Real-runtime behavior, including the default route everybody uses
// ---------------------------------------------------------------------------

static void RuntimeAllocation(ID3D11Device* device)
{
    // Every presentation format the source backend accepts must allocate, with
    // or without the capability. A format that cannot carry the binding falls
    // back rather than failing startup.
    const DXGI_FORMAT presentationFormats[]{
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT
    };
    for (const auto format : presentationFormats) {
        Targets targets;
        const auto output = PresentationDesc(format, 640, 360);
        Require(SUCCEEDED(targets.CreateUpscaleOutputAfterRetirement(device, output)) &&
                targets.UpscaleOutput() != nullptr,
            "every accepted presentation format allocates a handoff target");
        D3D11_TEXTURE2D_DESC published{};
        targets.UpscaleOutput()->GetDesc(&published);
        Require(published.Width == 640 && published.Height == 360 && published.Format == format,
            "the handoff target keeps the presentation extent and format");
        const bool capable = Targets::SupportsTypedUnorderedAccess(device, format);
        Require(HasUnorderedAccess(published) == capable,
            "the binding follows the capability the device actually reports");
        std::printf("  format=%d typedUAV=%s bind=0x%X\n",
            static_cast<int>(format), capable ? "yes" : "no", published.BindFlags);
    }

    // An sRGB presentation format cannot carry a typed unordered-access view.
    // The query keeps the binding from being requested at all here; the retry
    // would otherwise absorb the rejected allocation, so what this case really
    // pins down is that such a format still reaches a usable handoff target.
    {
        Targets targets;
        const auto output = PresentationDesc(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 640, 360);
        Require(!Targets::SupportsTypedUnorderedAccess(device, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
            "an sRGB presentation format reports no typed unordered-access support");
        Require(SUCCEEDED(targets.CreateUpscaleOutputAfterRetirement(device, output)) &&
                targets.UpscaleOutput() != nullptr,
            "an sRGB presentation format still allocates through the fallback");
        D3D11_TEXTURE2D_DESC published{};
        targets.UpscaleOutput()->GetDesc(&published);
        Require(published.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
            "an sRGB handoff target keeps exactly the proven bindings");
    }
}

// With both direct-output routes off the rendering path is unchanged: DLSS
// evaluates into its own texture and CopyResource moves the result into this
// target. The allocation now differs for those users even though the route does
// not, so the copy destination has to keep working with the added binding.
static void DefaultCopyRouteUnchanged(ID3D11Device* device, ID3D11DeviceContext* context)
{
    constexpr UINT width = 64, height = 32;
    const auto output = PresentationDesc(DXGI_FORMAT_R8G8B8A8_UNORM, width, height);

    Targets targets;
    Require(SUCCEEDED(targets.CreateUpscaleOutputAfterRetirement(device, output)) &&
            targets.UpscaleOutput() != nullptr,
        "the handoff target allocates for the default copy route");
    D3D11_TEXTURE2D_DESC published{};
    targets.UpscaleOutput()->GetDesc(&published);
    Require(Targets::SupportsTypedUnorderedAccess(device, DXGI_FORMAT_R8G8B8A8_UNORM) ==
            HasUnorderedAccess(published),
        "this run exercises the copy route against the binding the device reports");

    // Stand in for the texture DLSS evaluates into, with recognizable content.
    std::vector<std::uint32_t> scene(width * height);
    for (UINT i = 0; i < width * height; ++i) { scene[i] = 0xFF000000u | (i * 2654435761u & 0x00FFFFFFu); }
    auto sourceDesc = Targets::UpscaleOutputDesc(output);
    ComPtr<ID3D11Texture2D> source;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = scene.data();
    initial.SysMemPitch = width * sizeof(std::uint32_t);
    Require(SUCCEEDED(device->CreateTexture2D(&sourceDesc, &initial, &source)),
        "the evaluation source allocates at the presentation contract");

    // The operation the default route performs every frame.
    context->CopyResource(targets.UpscaleOutput(), source.Get());

    auto stagingDesc = published;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Require(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)), "readback staging texture");
    context->CopyResource(staging.Get(), targets.UpscaleOutput());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    Require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map the copied result");
    bool identical = true;
    for (UINT y = 0; y < height && identical; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
        identical = std::memcmp(row, scene.data() + y * width, width * sizeof(std::uint32_t)) == 0;
    }
    context->Unmap(staging.Get(), 0);
    Require(identical,
        "the default copy route reproduces the evaluated frame exactly with the new allocation");
}

int main()
{
    DescriptorContracts();
    AllocationDecision();

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device, nullptr, &context)),
        "WARP device for runtime allocation checks");
    RuntimeAllocation(device.Get());
    DefaultCopyRouteUnchanged(device.Get(), context.Get());

    std::printf("GameFacingTargets contracts, allocation decisions and default copy route hold\n");
    return 0;
}
