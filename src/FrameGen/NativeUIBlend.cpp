#include "NativeUIBlend.h"
#include <d3dcompiler.h>

namespace TheosRenderPipeline
{
    using Microsoft::WRL::ComPtr;
    bool NativeUIBlend::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, DXGI_FORMAT format)
    {
        ResetAfterRetirement();
        if (!device || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return false; }
        // Keep the compute route for sRGB, typeless and other unvalidated formats.
        if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
            format != DXGI_FORMAT_R32G32B32A32_FLOAT) { return false; }
        UINT support{};
        if (FAILED(device->CheckFormatSupport(format, &support)) ||
            (support & (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE)) !=
                (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE)) { return false; }
        ComPtr<ID3D11Device1> device1;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) ||
            FAILED(context->QueryInterface(IID_PPV_ARGS(&context1_)))) { return false; }
        const auto level = device->GetFeatureLevel();
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ?
            D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device), nullptr, &state_))) { return false; }
        constexpr char shader[] = R"(
float4 VS(uint i : SV_VertexID) : SV_Position {
    float2 p = float2((i << 1) & 2, i & 2);
    return float4(p * float2(2,-2) + float2(-1,1), 0, 1);
}
Texture2D<float4> Layer : register(t0);
float4 PS(float4 p : SV_Position) : SV_Target { return Layer.Load(int3(int2(p.xy),0)); }
)";
        ComPtr<ID3DBlob> vs, ps;
        if (FAILED(D3DCompile(shader, sizeof(shader)-1, "TRPNativeUIBlend", nullptr, nullptr,
                "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, nullptr)) ||
            FAILED(D3DCompile(shader, sizeof(shader)-1, "TRPNativeUIBlend", nullptr, nullptr,
                "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, nullptr)) ||
            FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_))) { return false; }
        D3D11_BLEND_DESC blend{}; auto& rt = blend.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
        if (FAILED(device->CreateBlendState(&blend, &blend_)) ||
            FAILED(device->CreateDepthStencilState(&depth, &depth_)) ||
            FAILED(device->CreateRasterizerState(&raster, &raster_))) { return false; }
        device_ = device; context_ = context; format_ = format;
        return true;
    }

    bool NativeUIBlend::Compose(ID3D11DeviceContext* context, ID3D11Texture2D* output,
        ID3D11ShaderResourceView* layer, UINT width, UINT height)
    {
        if (!device_ || context != context_.Get() || !output || !layer || !width || !height) { return false; }
        ComPtr<ID3D11Resource> layerResource;
        layer->GetResource(&layerResource);
        if (layerResource.Get() == output) { return false; }
        ComPtr<ID3D11Predicate> predicate; BOOL predicateValue{};
        context->GetPredication(&predicate, &predicateValue);
        if (predicate) { return false; } // Preserve the existing route for predicated work.
        ID3D11RenderTargetView* target = nullptr;
        for (auto& entry : targets_) {
            if (entry.texture.Get() == output) { target = entry.view.Get(); break; }
            if (!entry.texture) {
                D3D11_TEXTURE2D_DESC desc{}; output->GetDesc(&desc);
                if (desc.Width != width || desc.Height != height || desc.Format != format_ ||
                    desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1 ||
                    !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) { return false; }
                ComPtr<ID3D11RenderTargetView> view;
                if (FAILED(device_->CreateRenderTargetView(output, nullptr, &view))) { return false; }
                entry.texture = output; entry.view = view; target = view.Get(); break;
            }
        }
        if (!target) { return false; }
        // The runtime saves/restores all pipeline bindings, including implicitly
        // unbound aliases, OM UAVs, stream output, and shader class instances.
        ComPtr<ID3DDeviceContextState> previous;
        context1_->SwapDeviceContextState(state_.Get(), &previous);
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetBlendState(blend_.Get(), nullptr, ~0u);
        context->OMSetDepthStencilState(depth_.Get(), 0);
        context->RSSetState(raster_.Get());
        const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1}; context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_.Get(), nullptr, 0); context->PSSetShader(pixel_.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &layer); context->Draw(3, 0);
        // Do not retain per-frame attachments in the inactive private state.
        ID3D11ShaderResourceView* none = nullptr; context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context1_->SwapDeviceContextState(previous.Get(), nullptr);
        return true;
    }

    void NativeUIBlend::ResetAfterRetirement()
    {
        targets_ = {}; state_.Reset(); vertex_.Reset(); pixel_.Reset(); blend_.Reset(); depth_.Reset(); raster_.Reset();
        context1_.Reset(); context_.Reset(); device_.Reset(); format_ = DXGI_FORMAT_UNKNOWN;
    }
}
