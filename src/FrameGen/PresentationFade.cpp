#include "PresentationFade.h"
#include <d3dcompiler.h>

namespace TheosRenderPipeline
{
    using Microsoft::WRL::ComPtr;
    bool PresentationFade::Initialize(ID3D11DeviceContext* context)
    {
        if (state_) { return true; }
        if (failed_ || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return false; }
        failed_ = true; // Cleared only after every resource exists.
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11DeviceContext1> context1;
        if (!device || FAILED(device.As(&device1)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) { return false; }
        const auto level = device->GetFeatureLevel();
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ?
            D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        ComPtr<ID3DDeviceContextState> state;
        if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device), nullptr, &state))) { return false; }
        constexpr char shader[] = R"(
float4 VS(uint i : SV_VertexID) : SV_Position {
    float2 p = float2((i << 1) & 2, i & 2);
    return float4(p * float2(2,-2) + float2(-1,1), 0, 1);
}
float4 PS(float4 p : SV_Position) : SV_Target { return 0; }
)";
        ComPtr<ID3DBlob> vs, ps;
        ComPtr<ID3D11VertexShader> vertex; ComPtr<ID3D11PixelShader> pixel;
        if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "TRPPresentationFade", nullptr, nullptr,
                "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, nullptr)) ||
            FAILED(D3DCompile(shader, sizeof(shader) - 1, "TRPPresentationFade", nullptr, nullptr,
                "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, nullptr)) ||
            FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel))) { return false; }
        // dest.rgb * factor; alpha unchanged.
        D3D11_BLEND_DESC blendDesc{}; auto& rt = blendDesc.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ZERO; rt.DestBlend = D3D11_BLEND_BLEND_FACTOR; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC depthDesc{}; depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.DepthClipEnable = TRUE;
        ComPtr<ID3D11BlendState> blend; ComPtr<ID3D11DepthStencilState> depth; ComPtr<ID3D11RasterizerState> raster;
        if (FAILED(device->CreateBlendState(&blendDesc, &blend)) ||
            FAILED(device->CreateDepthStencilState(&depthDesc, &depth)) ||
            FAILED(device->CreateRasterizerState(&rasterDesc, &raster))) { return false; }
        device_ = device; context_ = context; context1_ = context1; vertex_ = vertex; pixel_ = pixel;
        blend_ = blend; depth_ = depth; raster_ = raster; state_ = state;
        failed_ = false;
        return true;
    }

    bool PresentationFade::Apply(ID3D11DeviceContext* context, ID3D11Texture2D* output, float factor)
    {
        if (!output || !(factor >= 0.0f && factor < 1.0f)) { return false; }
        if (!Initialize(context) || context != context_.Get()) { return false; }
        ComPtr<ID3D11Predicate> predicate; BOOL predicateValue{};
        context->GetPredication(&predicate, &predicateValue);
        if (predicate) { return false; }
        D3D11_TEXTURE2D_DESC desc{}; output->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1 ||
            !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) { return false; }
        UINT support{};
        if (FAILED(device_->CheckFormatSupport(desc.Format, &support)) || !(support & D3D11_FORMAT_SUPPORT_BLENDABLE)) { return false; }
        // Per call: a retained view of a swapchain buffer would block ResizeBuffers.
        ComPtr<ID3D11RenderTargetView> view;
        if (FAILED(device_->CreateRenderTargetView(output, nullptr, &view))) { return false; }
        ComPtr<ID3DDeviceContextState> previous;
        context1_->SwapDeviceContextState(state_.Get(), &previous);
        auto* target = view.Get();
        const float blendFactor[4]{ factor, factor, factor, 1.0f };
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetBlendState(blend_.Get(), blendFactor, ~0u);
        context->OMSetDepthStencilState(depth_.Get(), 0);
        context->RSSetState(raster_.Get());
        const D3D11_VIEWPORT viewport{ 0, 0, float(desc.Width), float(desc.Height), 0, 1 };
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_.Get(), nullptr, 0); context->PSSetShader(pixel_.Get(), nullptr, 0);
        context->Draw(3, 0);
        // Do not retain the frame's target in the inactive private state.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context1_->SwapDeviceContextState(previous.Get(), nullptr);
        return true;
    }

    void PresentationFade::ResetAfterRetirement() { *this = {}; }
}
