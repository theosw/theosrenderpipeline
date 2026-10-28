#include "FGInputDebugView.h"
#include "D3D11ComputeBindings.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace TheosRenderPipeline
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        struct Constants
        {
            float clipToPrevClip[16];
            std::uint32_t renderWidth, renderHeight, panelWidth, panelHeight;
            std::uint32_t depthInverted, padding[3];
        };
        static_assert(sizeof(Constants) % 16 == 0);
        constexpr char kShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 ClipToPrev;
    uint RenderWidth, RenderHeight, PanelWidth, PanelHeight;
    uint DepthInverted; uint3 Padding;
};
Texture2D<float4> Motion : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<unorm float4> Strip : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= PanelWidth * 3 || id.y >= PanelHeight) return;
    const uint panel = id.x / PanelWidth;
    const float2 uv = (float2(id.x % PanelWidth, id.y) + .5) / float2(PanelWidth, PanelHeight);
    const int2 p = min(int2(uv * float2(RenderWidth, RenderHeight)), int2(RenderWidth, RenderHeight) - 1);
    const float2 mv = Motion.Load(int3(p, 0)).xy;
    const float d = Depth.Load(int3(p, 0));
    const float e = DepthInverted ? d : 1 - d;  // proportional to 1 / view depth
    float3 c;
    if (panel == 0) {
        // Normalized screen motion; 1/32 of the screen reaches full colour.
        c = float3(saturate(.5 + mv * 16), .5);
    } else if (panel == 1) {
        c = saturate(log2(1 / max(e, 1e-7)) / 20).xxx;
    } else {
        // The motion the camera alone implies at this depth. The engine's sign
        // and y convention is not assumed: the closest of the four is used, so
        // a correct vector in any convention shows black.
        const float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
        const float4 prev = mul(clip, ClipToPrev);
        const float2 cam = prev.w != 0 ? float2(prev.x / prev.w * .5 + .5, .5 - prev.y / prev.w * .5) - uv : 0;
        const float2 px = float2(RenderWidth, RenderHeight);
        const float off = min(min(length((mv - cam) * px), length((mv + cam) * px)),
                              min(length((mv - float2(cam.x, -cam.y)) * px), length((mv + float2(cam.x, -cam.y)) * px)));
        c = all(isfinite(mv)) && isfinite(off) ? float3(saturate(off / 8), 0, 0) : float3(1, 0, 1);
    }
    Strip[id.xy] = float4(c, 1);
}
)";
        DXGI_FORMAT ReadFormat(DXGI_FORMAT format)
        {
            switch (format) {
            case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
            case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
            case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
            default: return format;
            }
        }
    }

    bool FGInputDebugView::Initialize(ID3D11DeviceContext* context)
    {
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        if (!device) { return false; }
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "TRPFGInputDebugView", nullptr, nullptr, "CS", "cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)) ||
            FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader_))) { return false; }
        D3D11_BUFFER_DESC buffer{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &constants_))) { return false; }
        device_ = device;
        return true;
    }

    ID3D11ShaderResourceView* FGInputDebugView::View(ID3D11Texture2D* texture, ComPtr<ID3D11Texture2D>& cached,
        ComPtr<ID3D11ShaderResourceView>& view)
    {
        if (cached.Get() == texture && view) { return view.Get(); }
        cached = texture; view.Reset();
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = ReadFormat(desc.Format); srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
        return SUCCEEDED(device_->CreateShaderResourceView(texture, &srv, &view)) ? view.Get() : nullptr;
    }

    void FGInputDebugView::Record(ID3D11DeviceContext* context, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
        const float (&clipToPrevClip)[16], bool depthInverted, std::uint32_t renderWidth, std::uint32_t renderHeight)
    {
        if (!Enabled() || failed_ || !context || !motion || !depth || !renderWidth || !renderHeight) { return; }
        if (!shader_ && !Initialize(context)) { failed_ = true; status_ = "debug view unavailable"; return; }
        if (!strip_) {
            // Created once and never replaced, so the overlay can hold the view safely.
            panelWidth_ = 480; panelHeight_ = (std::max)(1u, static_cast<std::uint32_t>(480ull * renderHeight / renderWidth));
            D3D11_TEXTURE2D_DESC desc{ panelWidth_ * 3, panelHeight_, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &strip_)) ||
                FAILED(device_->CreateUnorderedAccessView(strip_.Get(), nullptr, &stripUAV_)) ||
                FAILED(device_->CreateShaderResourceView(strip_.Get(), nullptr, &stripSRV_))) { failed_ = true; status_ = "debug view allocation failed"; return; }
        }
        auto* motionView = View(motion, motion_, motionSRV_);
        auto* depthView = View(depth, depth_, depthSRV_);
        if (!motionView || !depthView) { status_ = "debug view cannot read the inputs"; return; }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { return; }
        Constants c{};
        std::memcpy(c.clipToPrevClip, clipToPrevClip, sizeof(c.clipToPrevClip));
        c.renderWidth = renderWidth; c.renderHeight = renderHeight; c.panelWidth = panelWidth_; c.panelHeight = panelHeight_;
        c.depthInverted = depthInverted;
        std::memcpy(mapped.pData, &c, sizeof(c));
        context->Unmap(constants_.Get(), 0);
        {
        // The game's compute bindings are saved and put back; no state swap (see FGMotionRepair).
        const D3D11ComputeBindings saved(context, 1, 2, 1);
        ID3D11ShaderResourceView* inputs[2]{ motionView, depthView };
        auto* buffer = constants_.Get(); auto* uav = stripUAV_.Get();
        context->CSSetShader(shader_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &buffer);
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch((panelWidth_ * 3 + 7) / 8, (panelHeight_ + 7) / 8, 1);
        ID3D11ShaderResourceView* noInputs[2]{}; ID3D11UnorderedAccessView* noUAV{};
        context->CSSetShaderResources(0, 2, noInputs);
        context->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
        }
        status_ = "live";
    }
}
