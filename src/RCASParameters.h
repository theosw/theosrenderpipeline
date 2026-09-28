#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace TheosRenderPipeline
{
// Strength changes update one constant buffer, without rebuilding the shader.
class RCASParameters
{
public:
    HRESULT Update(ID3D11Device* device, ID3D11DeviceContext* context, float sharpness)
    {
        if (!device || !context) { return E_INVALIDARG; }
        const float value = std::isfinite(sharpness) ? std::clamp(sharpness, 0.0f, 1.0f) : 0.0f;
        if (buffer_ && device_ != device) { buffer_.Reset(); initialized_ = false; }
        if (buffer_ && initialized_ && value == lastValue_) { return S_OK; }
        if (!buffer_) {
            const D3D11_BUFFER_DESC desc{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                D3D11_CPU_ACCESS_WRITE, 0, 0};
            const auto hr = device->CreateBuffer(&desc, nullptr, &buffer_);
            if (FAILED(hr)) { return hr; }
            device_ = device;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr = context->Map(buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) { return hr; }
        const float constants[4]{value, 0, 0, 0};
        std::copy_n(constants, 4, static_cast<float*>(mapped.pData));
        context->Unmap(buffer_.Get(), 0);
        lastValue_ = value; initialized_ = true; ++uploads_;
        return S_OK;
    }
    class Binding
    {
    public:
        Binding(ID3D11DeviceContext* context, ID3D11Buffer* buffer) : context_(context)
        {
            if (SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(&context1_)))) {
                context1_->CSGetConstantBuffers1(0, 1, &previous_, &firstConstant_, &constantCount_);
            } else { context_->CSGetConstantBuffers(0, 1, &previous_); }
            context_->CSSetConstantBuffers(0, 1, &buffer);
        }
        ~Binding()
        {
            if (context1_) { context1_->CSSetConstantBuffers1(0, 1, &previous_, &firstConstant_, &constantCount_); }
            else { context_->CSSetConstantBuffers(0, 1, &previous_); }
            if (previous_) { previous_->Release(); }
        }
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;
    private:
        ID3D11DeviceContext* context_;
        ID3D11Buffer* previous_{};
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
        UINT firstConstant_{}, constantCount_{};
    };
    Binding Bind(ID3D11DeviceContext* context) const { return {context, buffer_.Get()}; }
    ID3D11Buffer* Buffer() const { return buffer_.Get(); }
    std::uint64_t Uploads() const { return uploads_; }
private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
    ID3D11Device* device_{}; // Retained by buffer_.
    float lastValue_{};
    bool initialized_{};
    std::uint64_t uploads_{};
};
}
