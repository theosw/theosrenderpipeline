#include "FGMotionRepair.h"
#include "D3D11ComputeBindings.h"
#include <d3dcompiler.h>
#include <cstring>
#include <utility>

namespace TheosRenderPipeline
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        constexpr std::uint32_t kGridX = 64, kGridY = 36;
        struct Constants
        {
            float clipToPrevClip[16];
            std::uint32_t renderWidth, renderHeight, depthInverted, padding;
        };
        static_assert(sizeof(Constants) % 16 == 0);
        // Skyrim's motion is previous minus current position in screen UV
        // (Community Shaders' MotionBlur::GetSSMotionVector), which is what
        // CameraMotion returns.
        constexpr char kShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 ClipToPrev;
    uint RenderWidth, RenderHeight, DepthInverted, Padding;
};
Texture2D<float4> Motion : register(t0);
Texture2D<float> Depth : register(t1);
RWByteAddressBuffer Counter : register(u0);
RWTexture2D<float2> Output : register(u1);
static const uint GridX = 64, GridY = 36;
float2 CameraMotion(uint2 p) {
    const float2 uv = (p + .5) / float2(RenderWidth, RenderHeight);
    const float4 prev = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, Depth.Load(int3(p, 0)), 1), ClipToPrev);
    return prev.w != 0 ? float2(prev.x / prev.w * .5 + .5, .5 - prev.y / prev.w * .5) - uv : 0;
}
[numthreads(8, 4, 1)] void Count(uint3 id : SV_DispatchThreadID) {
    if (id.x >= GridX || id.y >= GridY) return;
    const uint2 p = min(uint2((id.xy + .5) / float2(GridX, GridY) * float2(RenderWidth, RenderHeight)), uint2(RenderWidth, RenderHeight) - 1);
    const float2 off = (Motion.Load(int3(p, 0)).xy - CameraMotion(p)) * float2(RenderWidth, RenderHeight);
    if (!all(isfinite(off)) || length(off) > 4) Counter.InterlockedAdd(0, 1);
    // Diagnostic only: motion that varies across the frame is not a stuck frame.
    const float2 centre = Motion.Load(int3(RenderWidth / 2, RenderHeight / 2, 0)).xy;
    if (length((Motion.Load(int3(p, 0)).xy - centre) * float2(RenderWidth, RenderHeight)) > .5) Counter.InterlockedAdd(4, 1);
}
[numthreads(8, 8, 1)] void Write(uint3 id : SV_DispatchThreadID) {
    if (id.x >= RenderWidth || id.y >= RenderHeight) return;
    // Real motion disagrees with the camera only where objects move; a frame
    // where nearly every sample disagrees holds no real motion.
    const bool invalid = Counter.Load(0) * 10 > GridX * GridY * 9;
    const float2 camera = CameraMotion(id.xy);
    Output[id.xy] = invalid && all(isfinite(camera)) ? camera : Motion.Load(int3(id.xy, 0)).xy;
}
)";
        DXGI_FORMAT ReadFormat(DXGI_FORMAT format)
        {
            switch (format) {
            case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
            case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
            default: return format;
            }
        }
        ID3D11ShaderResourceView* View(ID3D11Device* device, ID3D11Texture2D* texture, ComPtr<ID3D11Texture2D>& cached,
            ComPtr<ID3D11ShaderResourceView>& view)
        {
            if (cached.Get() == texture && view) { return view.Get(); }
            cached = texture; view.Reset();
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = ReadFormat(desc.Format); srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
            return SUCCEEDED(device->CreateShaderResourceView(texture, &srv, &view)) ? view.Get() : nullptr;
        }
    }

    bool FGMotionRepair::Initialize(ID3D11DeviceContext* context)
    {
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        if (!device) { return false; }
        for (auto [entry, shader] : { std::pair{ "Count", &count_ }, std::pair{ "Write", &write_ } }) {
            ComPtr<ID3DBlob> blob;
            if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "TRPFGMotionRepair", nullptr, nullptr, entry, "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, nullptr)) ||
                FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader))) { return false; }
        }
        D3D11_BUFFER_DESC constants{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        D3D11_BUFFER_DESC counter{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
        D3D11_UNORDERED_ACCESS_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R32_TYPELESS; raw.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        raw.Buffer.NumElements = 4; raw.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        D3D11_BUFFER_DESC staging{ 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
        for (auto& buffer : readback_) {
            if (FAILED(device->CreateBuffer(&staging, nullptr, &buffer))) { return false; }
        }
        if (FAILED(device->CreateBuffer(&constants, nullptr, &constants_)) || FAILED(device->CreateBuffer(&counter, nullptr, &counter_)) ||
            FAILED(device->CreateUnorderedAccessView(counter_.Get(), &raw, &counterUAV_))) { return false; }
        device_ = device;
        return true;
    }

    bool FGMotionRepair::EnsureOutput(ID3D11Texture2D* motion)
    {
        D3D11_TEXTURE2D_DESC source{}; motion->GetDesc(&source);
        if (output_) {
            D3D11_TEXTURE2D_DESC current{}; output_->GetDesc(&current);
            if (current.Width == source.Width && current.Height == source.Height && current.Format == source.Format) { return true; }
        }
        // Same size and format as the engine's target, so frame generation copies it identically.
        const auto typed = ReadFormat(source.Format);
        if (typed != DXGI_FORMAT_R16G16_FLOAT && typed != DXGI_FORMAT_R32G32_FLOAT) { return false; }
        D3D11_TEXTURE2D_DESC desc{ source.Width, source.Height, 1, 1, source.Format, { 1, 0 }, D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = typed; uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        output_.Reset(); outputUAV_.Reset();
        return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &output_)) &&
            SUCCEEDED(device_->CreateUnorderedAccessView(output_.Get(), &uav, &outputUAV_));
    }

    ID3D11Texture2D* FGMotionRepair::Apply(ID3D11DeviceContext* context, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
        const float (&clipToPrevClip)[16], bool depthInverted, std::uint32_t renderWidth, std::uint32_t renderHeight,
        bool underwater)
    {
        if (!Enabled() || !underwater || failed_ || !context || !motion || !depth || !renderWidth || !renderHeight) { return motion; }
        if (!count_ && !Initialize(context)) { failed_ = true; return motion; }
        if (!EnsureOutput(motion)) { failed_ = true; return motion; }
        auto* motionView = View(device_.Get(), motion, motion_, motionSRV_);
        auto* depthView = View(device_.Get(), depth, depth_, depthSRV_);
        if (!motionView || !depthView) { return motion; }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { return motion; }
        Constants c{};
        std::memcpy(c.clipToPrevClip, clipToPrevClip, sizeof(c.clipToPrevClip));
        c.renderWidth = renderWidth; c.renderHeight = renderHeight; c.depthInverted = depthInverted;
        std::memcpy(mapped.pData, &c, sizeof(c));
        context->Unmap(constants_.Get(), 0);
        {
        // The game's compute bindings are saved and put back. No state swap: it
        // rewrites the context's dispatch table and drops the UI routing hooks.
        const D3D11ComputeBindings saved(context, 1, 2, 2);
        const UINT zero[4]{};
        context->ClearUnorderedAccessViewUint(counterUAV_.Get(), zero);
        ID3D11ShaderResourceView* inputs[2]{ motionView, depthView };
        ID3D11UnorderedAccessView* outputs[2]{ counterUAV_.Get(), outputUAV_.Get() };
        auto* buffer = constants_.Get();
        context->CSSetConstantBuffers(0, 1, &buffer);
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
        context->CSSetShader(count_.Get(), nullptr, 0);
        context->Dispatch((kGridX + 7) / 8, (kGridY + 3) / 4, 1);
        context->CSSetShader(write_.Get(), nullptr, 0);
        context->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
        ID3D11ShaderResourceView* noInputs[2]{}; ID3D11UnorderedAccessView* noOutputs[2]{};
        context->CSSetShaderResources(0, 2, noInputs);
        context->CSSetUnorderedAccessViews(0, 2, noOutputs, nullptr);
        }
        // Copy this frame's counts, then read the oldest copy if the GPU is done with it.
        const auto slot = applied_ % 3;
        context->CopyResource(readback_[slot].Get(), counter_.Get());
        readbackFrame_[slot] = ++applied_;
        const auto oldest = applied_ % 3;
        D3D11_MAPPED_SUBRESOURCE counts{};
        if (readbackFrame_[oldest] && SUCCEEDED(context->Map(readback_[oldest].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &counts))) {
            const auto* values = static_cast<const std::uint32_t*>(counts.pData);
            latest_ = { readbackFrame_[oldest], values[0], values[1], values[0] * 10 > kSamples * 9 };
            fresh_ = true;
            context->Unmap(readback_[oldest].Get(), 0);
            readbackFrame_[oldest] = 0;
        }
        return output_.Get();
    }

    bool FGMotionRepair::TakeDecision(Decision& out)
    {
        if (!fresh_) { return false; }
        fresh_ = false;
        out = latest_;
        return true;
    }
}
