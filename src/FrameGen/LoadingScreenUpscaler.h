#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace TheosRenderPipeline
{
    // Loading backgrounds use spatial scaling without temporal inputs or sharpening.
    // The owner releases these views/resources only after host retirement.
    class LoadingScreenUpscaler
    {
        template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
        Ptr<ID3D11ComputeShader> shader_;
        Ptr<ID3D11Texture2D> source_, result_;
        Ptr<ID3D11ShaderResourceView> srv_;
        Ptr<ID3D11UnorderedAccessView> uav_;
        UINT inputWidth_{}, inputHeight_{}, outputWidth_{}, outputHeight_{};

        HRESULT Initialize(ID3D11Device* device, UINT iw, UINT ih, UINT ow, UINT oh)
        {
            static constexpr char source[] = R"(
Texture2D<float4> inputImage : register(t0);
RWTexture2D<float4> outputImage : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint iw, ih, ow, oh;
    inputImage.GetDimensions(iw, ih); outputImage.GetDimensions(ow, oh);
    if (id.x >= ow || id.y >= oh) return;
    float2 p = (float2(id.xy) + 0.5) * float2(iw, ih) / float2(ow, oh) - 0.5;
    int2 lo = int2(floor(p)), limit = int2(iw, ih) - 1;
    float2 f = frac(p);
    float4 a = inputImage.Load(int3(clamp(lo, int2(0,0), limit), 0));
    float4 b = inputImage.Load(int3(clamp(lo + int2(1,0), int2(0,0), limit), 0));
    float4 c = inputImage.Load(int3(clamp(lo + int2(0,1), int2(0,0), limit), 0));
    float4 d = inputImage.Load(int3(clamp(lo + int2(1,1), int2(0,0), limit), 0));
    outputImage[id.xy] = lerp(lerp(a,b,f.x), lerp(c,d,f.x), f.y);
})";
            Ptr<ID3DBlob> code, errors;
            auto hr = D3DCompile(source, sizeof(source)-1, "LoadingScreenUpscaler", nullptr, nullptr,
                "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
            if (FAILED(hr)) { return hr; }
            Ptr<ID3D11ComputeShader> shader;
            hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
            if (FAILED(hr)) { return hr; }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = iw; desc.Height = ih; desc.MipLevels = desc.ArraySize = 1;
            desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            Ptr<ID3D11Texture2D> input, output;
            Ptr<ID3D11ShaderResourceView> srv; Ptr<ID3D11UnorderedAccessView> uav;
            hr = device->CreateTexture2D(&desc, nullptr, &input);
            if (FAILED(hr)) { return hr; }
            hr = device->CreateShaderResourceView(input.Get(), nullptr, &srv);
            if (FAILED(hr)) { return hr; }
            desc.Width = ow; desc.Height = oh; desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            hr = device->CreateTexture2D(&desc, nullptr, &output);
            if (FAILED(hr)) { return hr; }
            hr = device->CreateUnorderedAccessView(output.Get(), nullptr, &uav);
            if (FAILED(hr)) { return hr; }
            shader_ = shader; source_ = input; result_ = output; srv_ = srv; uav_ = uav;
            inputWidth_ = iw; inputHeight_ = ih; outputWidth_ = ow; outputHeight_ = oh;
            return S_OK;
        }
    public:
        HRESULT Evaluate(ID3D11DeviceContext* context, ID3D11Texture2D* input, ID3D11Texture2D* output)
        {
            if (!context || !input || !output || input == output ||
                context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return E_INVALIDARG; }
            D3D11_TEXTURE2D_DESC in{}, out{}; input->GetDesc(&in); output->GetDesc(&out);
            // The game-facing upscaler resources use single-sample RGBA8 textures.
            if (in.Format != DXGI_FORMAT_R8G8B8A8_UNORM || out.Format != in.Format ||
                in.MipLevels != 1 || out.MipLevels != 1 || in.ArraySize != 1 || out.ArraySize != 1 ||
                in.SampleDesc.Count != 1 || out.SampleDesc.Count != 1) { return E_INVALIDARG; }
            if (!shader_) {
                Ptr<ID3D11Device> device; context->GetDevice(&device);
                const auto hr = Initialize(device.Get(), in.Width, in.Height, out.Width, out.Height);
                if (FAILED(hr)) { return hr; }
            }
            if (in.Width != inputWidth_ || in.Height != inputHeight_ || out.Width != outputWidth_ ||
                out.Height != outputHeight_) { return E_INVALIDARG; }
            Ptr<ID3D11ComputeShader> oldShader;
            std::array<ID3D11ClassInstance*, 256> instances{}; UINT count = static_cast<UINT>(instances.size());
            context->CSGetShader(&oldShader, instances.data(), &count);
            Ptr<ID3D11ShaderResourceView> oldSRV; Ptr<ID3D11UnorderedAccessView> oldUAV;
            context->CSGetShaderResources(0, 1, &oldSRV);
            context->CSGetUnorderedAccessViews(0, 1, &oldUAV);
            // Owned intermediate textures avoid automatic unbinding of the producer's resources in other stages.
            context->CopyResource(source_.Get(), input);
            auto* srv = srv_.Get(); auto* uav = uav_.Get(); const UINT keepCounter = ~UINT{};
            context->CSSetShader(shader_.Get(), nullptr, 0);
            context->CSSetShaderResources(0, 1, &srv);
            context->CSSetUnorderedAccessViews(0, 1, &uav, &keepCounter);
            context->Dispatch((out.Width + 7) / 8, (out.Height + 7) / 8, 1);
            ID3D11ShaderResourceView* nullSRV{}; ID3D11UnorderedAccessView* nullUAV{};
            context->CSSetShaderResources(0, 1, &nullSRV);
            context->CSSetUnorderedAccessViews(0, 1, &nullUAV, &keepCounter);
            context->CopyResource(output, result_.Get());
            srv = oldSRV.Get(); uav = oldUAV.Get();
            context->CSSetShaderResources(0, 1, &srv);
            context->CSSetUnorderedAccessViews(0, 1, &uav, &keepCounter);
            context->CSSetShader(oldShader.Get(), instances.data(), count);
            for (UINT i = 0; i < count; ++i) { if (instances[i]) { instances[i]->Release(); } }
            return S_OK;
        }
        void ResetAfterRetirement() { *this = {}; }
    };
}
