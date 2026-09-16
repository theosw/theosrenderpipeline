#include "InventoryPreviewDraw.h"
#include <array>
#include <new>
#include <d3dcompiler.h>
#include <cstring>

namespace TheosRenderPipeline
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        D3D11_BLEND_DESC BlendDescription(ID3D11BlendState* state)
        {
            D3D11_BLEND_DESC desc{};
            if (state) { state->GetDesc(&desc); }
            else {
                auto& rt = desc.RenderTarget[0];
                rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
                rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            }
            if (!desc.IndependentBlendEnable) {
                for (size_t i = 1; i < 8; ++i) { desc.RenderTarget[i] = desc.RenderTarget[0]; }
                desc.IndependentBlendEnable = TRUE;
            }
            return desc;
        }

        struct Bindings
        {
            ID3D11DeviceContext* context;
            std::array<ComPtr<ID3D11RenderTargetView>, 8> targets;
            ComPtr<ID3D11DepthStencilView> dsv;
            ComPtr<ID3D11BlendState> blend;
            ComPtr<ID3D11DepthStencilState> depth;
            FLOAT factors[4]{};
            UINT sampleMask{}, stencil{}, count{};
            bool changed{};

            explicit Bindings(ID3D11DeviceContext* value) : context(value)
            {
                ID3D11RenderTargetView* raw[8]{};
                context->OMGetRenderTargets(8, raw, &dsv);
                for (UINT i = 0; i < 8; ++i) {
                    targets[i].Attach(raw[i]);
                    if (raw[i]) { count = i + 1; }
                }
                context->OMGetBlendState(&blend, factors, &sampleMask);
                context->OMGetDepthStencilState(&depth, &stencil);
            }
            void RestoreTargets() const
            {
                ID3D11RenderTargetView* raw[8]{};
                for (UINT i = 0; i < count; ++i) { raw[i] = targets[i].Get(); }
                context->OMSetRenderTargets(count, raw, dsv.Get());
                context->OMSetDepthStencilState(depth.Get(), stencil);
            }
            ~Bindings()
            {
                if (!changed) { return; }
                RestoreTargets();
                context->OMSetBlendState(blend.Get(), factors, sampleMask);
            }
        };

        // The alpha stamp touches only these shader/raster/IA bindings. Keep
        // class instances as well as shader identities for linked shaders.
        template<class Shader> struct ShaderBinding
        {
            ComPtr<Shader> shader;
            ID3D11ClassInstance* instances[256]{};
            UINT count{256};
            ~ShaderBinding() { for (UINT i = 0; i < count; ++i) { if (instances[i]) { instances[i]->Release(); } } }
        };

        struct StampBindings
        {
            ID3D11DeviceContext* context;
            ShaderBinding<ID3D11VertexShader> vs;
            ShaderBinding<ID3D11PixelShader> ps;
            ShaderBinding<ID3D11GeometryShader> gs;
            ShaderBinding<ID3D11HullShader> hs;
            ShaderBinding<ID3D11DomainShader> ds;
            ComPtr<ID3D11InputLayout> layout;
            ComPtr<ID3D11RasterizerState> raster;
            ComPtr<ID3D11ShaderResourceView> srv;
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT viewportCount{D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
            explicit StampBindings(ID3D11DeviceContext* value) : context(value)
            {
                context->VSGetShader(&vs.shader, vs.instances, &vs.count);
                context->PSGetShader(&ps.shader, ps.instances, &ps.count);
                context->GSGetShader(&gs.shader, gs.instances, &gs.count);
                context->HSGetShader(&hs.shader, hs.instances, &hs.count);
                context->DSGetShader(&ds.shader, ds.instances, &ds.count);
                context->IAGetInputLayout(&layout);
                context->IAGetPrimitiveTopology(&topology);
                context->RSGetState(&raster);
                context->RSGetViewports(&viewportCount, viewports);
                context->PSGetShaderResources(0, 1, &srv);
            }
            ~StampBindings()
            {
                context->VSSetShader(vs.shader.Get(), vs.instances, vs.count);
                context->PSSetShader(ps.shader.Get(), ps.instances, ps.count);
                context->GSSetShader(gs.shader.Get(), gs.instances, gs.count);
                context->HSSetShader(hs.shader.Get(), hs.instances, hs.count);
                context->DSSetShader(ds.shader.Get(), ds.instances, ds.count);
                context->IASetInputLayout(layout.Get());
                context->IASetPrimitiveTopology(topology);
                context->RSSetState(raster.Get());
                context->RSSetViewports(viewportCount, viewports);
                auto* view = srv.Get(); context->PSSetShaderResources(0, 1, &view);
            }
        };

        struct InternalScope
        {
            bool& flag;
            explicit InternalScope(bool& value) : flag(value) { flag = true; }
            ~InternalScope() { flag = false; }
        };

        bool HasReplaySideEffects(ID3D11DeviceContext* context)
        {
            // Replaying a conventional mesh shader preserves discard/coverage.
            // A shader that writes UAVs or stream output must not run twice.
            ID3D11UnorderedAccessView* uavs[8]{};
            context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uavs);
            bool found = false;
            for (auto* view : uavs) { if (view) { found = true; view->Release(); } }
            ID3D11Buffer* buffers[4]{};
            context->SOGetTargets(4, buffers);
            for (auto* buffer : buffers) { if (buffer) { found = true; buffer->Release(); } }
            return found;
        }
    }

    InventoryPreviewDraw::Blend& InventoryPreviewDraw::ResolveBlend(ID3D11Device* device, ID3D11BlendState* original)
    {
        for (auto& entry : blends_) { if (entry.original.Get() == original) { return entry; } }
        auto& entry = blends_.emplace_back();
        entry.original = original;
        auto desc = BlendDescription(original);
        auto& rt = desc.RenderTarget[0];
        if (desc.AlphaToCoverageEnable || rt.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL) { return entry; }
        // Do not reinterpret an existing logic operation as ordinary blending.
        ComPtr<ID3D11BlendState1> extended;
        if (original && SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&extended)))) {
            D3D11_BLEND_DESC1 detail{}; extended->GetDesc1(&detail);
            if (detail.RenderTarget[0].LogicOpEnable) { return entry; }
        }
        entry.opaque = !rt.BlendEnable;
        if (entry.opaque) {
            rt.RenderTargetWriteMask &= ~D3D11_COLOR_WRITE_ENABLE_ALPHA;
        } else if (rt.BlendOp == D3D11_BLEND_OP_ADD &&
            (rt.SrcBlend == D3D11_BLEND_SRC_ALPHA || rt.SrcBlend == D3D11_BLEND_ONE) &&
            (rt.DestBlend == D3D11_BLEND_INV_SRC_ALPHA || rt.DestBlend == D3D11_BLEND_ONE)) {
            // Source-over contributes coverage a, not a*a. Additive light does
            // not occlude the scene and therefore preserves existing coverage.
            rt.SrcBlendAlpha = rt.DestBlend == D3D11_BLEND_ONE ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
            rt.DestBlendAlpha = rt.DestBlend;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        } else { return entry; }
        entry.result = device->CreateBlendState(&desc, &entry.corrected);
        return entry;
    }

    InventoryPreviewDraw::Depth& InventoryPreviewDraw::ResolveDepth(ID3D11Device* device, ID3D11DepthStencilState* original)
    {
        for (auto& entry : depths_) { if (entry.original.Get() == original) { return entry; } }
        auto& entry = depths_.emplace_back();
        entry.original = original;
        D3D11_DEPTH_STENCIL_DESC desc{};
        if (original) { original->GetDesc(&desc); }
        else {
            desc.DepthEnable = TRUE; desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            desc.DepthFunc = D3D11_COMPARISON_LESS;
            desc.StencilReadMask = desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_READ_MASK;
            desc.FrontFace = desc.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        }
        // If failed fragments mutate stencil, a later fragment could become
        // eligible only because of that mutation. Such a draw cannot be probed
        // with a read-only pass and is left unchanged.
        if (desc.StencilEnable && (desc.FrontFace.StencilFailOp != D3D11_STENCIL_OP_KEEP ||
            desc.BackFace.StencilFailOp != D3D11_STENCIL_OP_KEEP ||
            desc.FrontFace.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP ||
            desc.BackFace.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP)) { return entry; }
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.StencilWriteMask = 0;
        desc.FrontFace.StencilPassOp = desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        entry.result = device->CreateDepthStencilState(&desc, &entry.readOnly);
        return entry;
    }

    HRESULT InventoryPreviewDraw::EnsureCoverage(ID3D11Device* device, UINT width, UINT height)
    {
        if (mask_) { return width == width_ && height == height_ ? S_OK : E_UNEXPECTED; }
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        auto hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
        if (FAILED(hr)) { return hr; }
        if (!options.OutputMergerLogicOp) { return DXGI_ERROR_UNSUPPORTED; }
        ComPtr<ID3D11Device1> device1;
        hr = device->QueryInterface(IID_PPV_ARGS(&device1));
        if (FAILED(hr)) { return hr; }
        ComPtr<ID3D11BlendState1> coverage;
        D3D11_BLEND_DESC1 desc{};
        auto& rt = desc.RenderTarget[0];
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.LogicOpEnable = TRUE; rt.LogicOp = D3D11_LOGIC_OP_SET;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
        hr = device1->CreateBlendState1(&desc, &coverage);
        if (FAILED(hr)) { return hr; }

        // Logic SET requires a UINT target. Running the original shader here
        // retains its discard behavior; its output bits do not affect SET.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8_UINT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> mask;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(hr = device->CreateTexture2D(&td, nullptr, &mask)) ||
            FAILED(hr = device->CreateRenderTargetView(mask.Get(), nullptr, &rtv)) ||
            FAILED(hr = device->CreateShaderResourceView(mask.Get(), nullptr, &srv))) { return hr; }
        constexpr char vertex[] =
            "float4 main(uint id:SV_VertexID):SV_Position {"
            "float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); }";
        constexpr char pixel[] =
            "Texture2D<uint> coverage:register(t0); float4 main(float4 p:SV_Position):SV_Target {"
            "if(coverage.Load(int3(p.xy,0))==0) discard; return float4(0,0,0,1); }";
        ComPtr<ID3DBlob> vsCode, psCode;
        if (FAILED(hr = D3DCompile(vertex, std::strlen(vertex), "Inventory coverage", nullptr, nullptr,
            "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, nullptr)) ||
            FAILED(hr = D3DCompile(pixel, std::strlen(pixel), "Inventory coverage", nullptr, nullptr,
            "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, nullptr))) { return hr; }
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        if (FAILED(hr = device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) ||
            FAILED(hr = device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps))) { return hr; }
        auto blendDesc = BlendDescription(nullptr);
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        ComPtr<ID3D11BlendState> blend;
        if (FAILED(hr = device->CreateBlendState(&blendDesc, &blend))) { return hr; }
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
            D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        ComPtr<ID3D11DepthStencilState> depth;
        if (FAILED(hr = device->CreateDepthStencilState(&dd, &depth))) { return hr; }
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        if (FAILED(hr = device->CreateRasterizerState(&rd, &raster))) { return hr; }
        coverage_ = coverage; mask_ = mask; maskRTV_ = rtv; maskSRV_ = srv;
        stampVS_ = vs; stampPS_ = ps; stampBlend_ = blend; stampDepth_ = depth; stampRaster_ = raster;
        width_ = width; height_ = height;
        return S_OK;
    }

    HRESULT InventoryPreviewDraw::Draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* nativeUI,
        const std::function<void()>& original, NativeUIDepthInputs depthInputs)
    {
        depthSubstitutions_ = 0;
        if (!context || !nativeUI || internal_) { original(); return S_FALSE; }
        InternalScope internal(internal_);
        Bindings saved(context);
        if (saved.targets[0].Get() != nativeUI) { original(); return S_FALSE; }
        NativeUIDepthScope depthBindings(context, depthInputs);
        depthSubstitutions_ = depthBindings.Count();
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        Blend* blend{}; Depth* depth{};
        HRESULT result = S_OK;
        try {
            blend = &ResolveBlend(device.Get(), saved.blend.Get());
            result = blend->result;
            if (result == S_OK && blend->opaque) {
                ComPtr<ID3D11Resource> resource; nativeUI->GetResource(&resource);
                ComPtr<ID3D11Texture2D> texture;
                result = resource.As(&texture);
                if (SUCCEEDED(result)) {
                    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                    result = desc.SampleDesc.Count == 1 && !HasReplaySideEffects(context) ?
                        EnsureCoverage(device.Get(), desc.Width, desc.Height) : DXGI_ERROR_UNSUPPORTED;
                }
                if (SUCCEEDED(result)) { depth = &ResolveDepth(device.Get(), saved.depth.Get()); result = depth->result; }
            }
        } catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; }
        if (result != S_OK) { original(); return result; }
        saved.changed = true;
        if (blend->opaque) {
            // Run the original shader, including discard, through the original
            // tests without modifying depth/stencil or any RGB/MRT output.
            const FLOAT clear[4]{};
            context->ClearRenderTargetView(maskRTV_.Get(), clear);
            auto* maskTarget = maskRTV_.Get();
            context->OMSetRenderTargets(1, &maskTarget, saved.dsv.Get());
            context->OMSetDepthStencilState(depth->readOnly.Get(), saved.stencil);
            context->OMSetBlendState(coverage_.Get(), saved.factors, saved.sampleMask);
            original();
            {
                StampBindings stamp(context);
                context->OMSetRenderTargets(1, &nativeUI, nullptr);
                context->OMSetDepthStencilState(stampDepth_.Get(), 0);
                context->OMSetBlendState(stampBlend_.Get(), nullptr, UINT_MAX);
                context->VSSetShader(stampVS_.Get(), nullptr, 0);
                context->PSSetShader(stampPS_.Get(), nullptr, 0);
                context->GSSetShader(nullptr, nullptr, 0);
                context->HSSetShader(nullptr, nullptr, 0);
                context->DSSetShader(nullptr, nullptr, 0);
                context->IASetInputLayout(nullptr);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->RSSetState(stampRaster_.Get());
                const D3D11_VIEWPORT viewport{0, 0, float(width_), float(height_), 0, 1};
                context->RSSetViewports(1, &viewport);
                auto* view = maskSRV_.Get(); context->PSSetShaderResources(0, 1, &view);
                context->Draw(3, 0);
            }
            saved.RestoreTargets();
        }
        context->OMSetBlendState(blend->corrected.Get(), saved.factors, saved.sampleMask);
        original();
        return S_OK;
    }

    void InventoryPreviewDraw::ResetAfterRetirement()
    {
        blends_.clear(); depths_.clear(); coverage_.Reset();
        mask_.Reset(); maskRTV_.Reset(); maskSRV_.Reset();
        stampVS_.Reset(); stampPS_.Reset(); stampBlend_.Reset(); stampDepth_.Reset(); stampRaster_.Reset();
        width_ = height_ = 0;
    }
}
