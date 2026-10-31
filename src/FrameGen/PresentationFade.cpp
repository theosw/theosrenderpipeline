#include "PresentationFade.h"
#include <d3dcompiler.h>
#include <array>

namespace TheosRenderPipeline
{
    using Microsoft::WRL::ComPtr;
    bool PresentationFade::Initialize(ID3D11DeviceContext* context)
    {
        if (vertex_) { return true; }
        if (failed_ || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return false; }
        failed_ = true; // Cleared only after every resource exists.
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        if (!device) { return false; }
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
        device_ = device; context_ = context; vertex_ = vertex; pixel_ = pixel;
        blend_ = blend; depth_ = depth; raster_ = raster;
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
        // State swaps rewrite native dispatch tables and erase the UI hooks.
        // Save the bindings this graphics draw touches, including resources the
        // runtime may unbind when our output becomes a render target.
        std::array<ID3D11RenderTargetView*,8> targets{};
        ComPtr<ID3D11DepthStencilView> dsv;
        context->OMGetRenderTargets(8,targets.data(),&dsv);
        UINT targetCount=8; while(targetCount && !targets[targetCount-1]) { --targetCount; }
        const UINT uavCount=device_->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
        std::array<ID3D11UnorderedAccessView*,64> omUAVs{}, csUAVs{};
        context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,uavCount,omUAVs.data());
        context->CSGetUnorderedAccessViews(0,uavCount,csUAVs.data());
        ComPtr<ID3D11BlendState> blend; FLOAT factors[4]{}; UINT mask{};
        context->OMGetBlendState(&blend,factors,&mask);
        ComPtr<ID3D11DepthStencilState> depth; UINT stencil{};
        context->OMGetDepthStencilState(&depth,&stencil);
        ComPtr<ID3D11RasterizerState> raster; context->RSGetState(&raster);
        std::array<D3D11_VIEWPORT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        UINT viewportCount=static_cast<UINT>(viewports.size()); context->RSGetViewports(&viewportCount,viewports.data());
        D3D11_PRIMITIVE_TOPOLOGY topology{}; context->IAGetPrimitiveTopology(&topology);
        std::array<ID3D11Buffer*,4> streamOutput{}; context->SOGetTargets(4,streamOutput.data());
#define SAVE_STAGE(type, prefix) \
        ComPtr<ID3D11##type##Shader> saved##prefix; \
        std::array<ID3D11ClassInstance*,256> classes##prefix{}; UINT count##prefix=256; \
        std::array<ID3D11ShaderResourceView*,128> views##prefix{}; \
        context->prefix##GetShader(&saved##prefix,classes##prefix.data(),&count##prefix); \
        context->prefix##GetShaderResources(0,128,views##prefix.data());
        SAVE_STAGE(Vertex,VS) SAVE_STAGE(Pixel,PS) SAVE_STAGE(Geometry,GS) SAVE_STAGE(Hull,HS) SAVE_STAGE(Domain,DS)
#undef SAVE_STAGE
        std::array<ID3D11ShaderResourceView*,128> viewsCS{};
        context->CSGetShaderResources(0,128,viewsCS.data());
        context->SOSetTargets(0,nullptr,nullptr);
        context->GSSetShader(nullptr,nullptr,0); context->HSSetShader(nullptr,nullptr,0); context->DSSetShader(nullptr,nullptr,0);
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
        context->OMSetRenderTargets(0,nullptr,nullptr);
        std::array<UINT,64> keep; keep.fill(~0u);
        context->CSSetUnorderedAccessViews(0,uavCount,csUAVs.data(),keep.data());
        context->OMSetRenderTargetsAndUnorderedAccessViews(targetCount,targets.data(),dsv.Get(),targetCount,
            uavCount-targetCount,omUAVs.data()+targetCount,keep.data());
        context->OMSetBlendState(blend.Get(),factors,mask); context->OMSetDepthStencilState(depth.Get(),stencil);
        context->RSSetState(raster.Get()); context->RSSetViewports(viewportCount,viewports.data());
        context->IASetPrimitiveTopology(topology);
        // Append restores the saved SO cursors; no fade draw touched these buffers.
        context->SOSetTargets(4,streamOutput.data(),keep.data());
#define RESTORE_STAGE(prefix) \
        context->prefix##SetShader(saved##prefix.Get(),classes##prefix.data(),count##prefix); \
        context->prefix##SetShaderResources(0,128,views##prefix.data()); \
        for(auto* value:classes##prefix) { if(value) { value->Release(); } } \
        for(auto* value:views##prefix) { if(value) { value->Release(); } }
        RESTORE_STAGE(VS) RESTORE_STAGE(PS) RESTORE_STAGE(GS) RESTORE_STAGE(HS) RESTORE_STAGE(DS)
#undef RESTORE_STAGE
        context->CSSetShaderResources(0,128,viewsCS.data());
        for(auto* value:viewsCS) { if(value) { value->Release(); } }
        for(auto* value:targets) { if(value) { value->Release(); } }
        for(auto* value:omUAVs) { if(value) { value->Release(); } }
        for(auto* value:csUAVs) { if(value) { value->Release(); } }
        for(auto* value:streamOutput) { if(value) { value->Release(); } }
        return true;
    }

    void PresentationFade::ResetAfterRetirement() { *this = {}; }
}
