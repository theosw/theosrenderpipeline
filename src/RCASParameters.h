#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

namespace TheosRenderPipeline
{
// Strength changes update one constant buffer, without rebuilding the shader.
class RCASParameters
{
public:
    HRESULT Update(ID3D11Device* device, ID3D11DeviceContext* context, float sharpness)
    {
        if (!device || !context) { return E_INVALIDARG; }
        if (!buffer_) {
            const D3D11_BUFFER_DESC desc{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                D3D11_CPU_ACCESS_WRITE, 0, 0};
            const auto hr = device->CreateBuffer(&desc, nullptr, &buffer_);
            if (FAILED(hr)) { return hr; }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr = context->Map(buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) { return hr; }
        const float value = std::isfinite(sharpness) ? std::clamp(sharpness, 0.0f, 1.0f) : 0.0f;
        const float constants[4]{value, 0, 0, 0};
        std::copy_n(constants, 4, static_cast<float*>(mapped.pData));
        context->Unmap(buffer_.Get(), 0);
        return S_OK;
    }
    class Binding
    {
    public:
        Binding(ID3D11DeviceContext* context, ID3D11Buffer* buffer) : context_(context)
        {
            context_->CSGetConstantBuffers(0, 1, &previous_);
            context_->CSSetConstantBuffers(0, 1, &buffer);
        }
        ~Binding() { context_->CSSetConstantBuffers(0, 1, &previous_); if (previous_) { previous_->Release(); } }
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;
    private:
        ID3D11DeviceContext* context_;
        ID3D11Buffer* previous_{};
    };
    Binding Bind(ID3D11DeviceContext* context) const { return {context, buffer_.Get()}; }
    ID3D11Buffer* Buffer() const { return buffer_.Get(); }
private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
};
}
