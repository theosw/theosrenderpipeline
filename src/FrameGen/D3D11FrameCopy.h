#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <utility>

namespace TheosRenderPipeline
{
    struct FrameExtent
    {
        UINT width{}, height{};
        bool Fits(const D3D11_TEXTURE2D_DESC& desc) const
        {
            return width && height && width <= desc.Width && height <= desc.Height &&
                desc.MipLevels == 1 && desc.ArraySize == 1 &&
                desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0;
        }
    };

    namespace D3D11FrameCopy
    {
        using Microsoft::WRL::ComPtr;

        inline bool SameObject(IUnknown* first, IUnknown* second)
        {
            if (!first || !second) { return false; }
            ComPtr<IUnknown> a, b;
            return SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(&a))) &&
                SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
        }

        inline bool ValidResources(ID3D11DeviceContext* context, ID3D11Texture2D* source,
            ID3D11Texture2D* destination)
        {
            if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
                !source || !destination || SameObject(source, destination)) { return false; }
            ComPtr<ID3D11Device> device, sourceDevice, destinationDevice;
            context->GetDevice(&device); source->GetDevice(&sourceDevice); destination->GetDevice(&destinationDevice);
            return SameObject(device.Get(), sourceDevice.Get()) && SameObject(device.Get(), destinationDevice.Get());
        }

        // Both rectangles start at (0, 0). Allocation size is independent of the
        // active render area. Copying back into a larger producer texture leaves
        // its unused border intact. Callers order GPU readers before these writes.
        inline HRESULT Color(ID3D11DeviceContext* context, ID3D11Texture2D* source,
            ID3D11Texture2D* destination, FrameExtent extent)
        {
            if (!ValidResources(context, source, destination)) { return E_INVALIDARG; }
            D3D11_TEXTURE2D_DESC from{}, to{}; source->GetDesc(&from); destination->GetDesc(&to);
            if (!extent.Fits(from) || !extent.Fits(to) || from.Format != to.Format ||
                (from.BindFlags & D3D11_BIND_DEPTH_STENCIL) || (to.BindFlags & D3D11_BIND_DEPTH_STENCIL) ||
                to.Usage == D3D11_USAGE_IMMUTABLE) { return E_INVALIDARG; }
            if (extent.width == from.Width && extent.height == from.Height &&
                extent.width == to.Width && extent.height == to.Height) {
                context->CopyResource(destination, source);
            } else {
                const D3D11_BOX box{0, 0, 0, extent.width, extent.height, 1};
                context->CopySubresourceRegion(destination, 0, 0, 0, 0, source, 0, &box);
            }
            return S_OK;
        }

        // Depth-stencil resources cannot use a partial CopySubresourceRegion.
        // Read the active rectangle through an SRV into a tightly sized R32 view.
        // The producer must unbind writable source/destination views first;
        // only CS shader, SRV 0 and UAV 0 are touched and restored here.
        class Depth final
        {
        public:
            void ResetViews() { source_.Reset(); destination_.Reset(); srv_.Reset(); uav_.Reset(); }

            HRESULT Copy(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                ID3D11Texture2D* destination, FrameExtent extent)
            {
                if (!ValidResources(context, source, destination)) { return E_INVALIDARG; }
                D3D11_TEXTURE2D_DESC from{}, to{}; source->GetDesc(&from); destination->GetDesc(&to);
                if (!extent.Fits(from) || !extent.Fits(to) || to.Width != extent.width || to.Height != extent.height ||
                    to.Format != DXGI_FORMAT_R32_FLOAT || !(from.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                    !(to.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) { return E_INVALIDARG; }
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                switch (from.Format) {
                case DXGI_FORMAT_R24G8_TYPELESS: view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
                case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: view.Format = DXGI_FORMAT_R32_FLOAT; break;
                default: return E_INVALIDARG;
                }
                view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
                ComPtr<ID3D11Device> device; context->GetDevice(&device);
                if (shader_) {
                    // ReShade can return its native device from a shader's
                    // GetDevice while contexts/resources expose the proxy.
                    // Retain the device that actually created our shader;
                    // never infer ownership from an incoming resource.
                    if (!SameObject(device.Get(), shaderDevice_.Get())) { return E_INVALIDARG; }
                } else {
                    constexpr char program[] = "Texture2D<float> s:register(t0); RWTexture2D<float> d:register(u0); [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){uint w,h; d.GetDimensions(w,h); if(p.x<w && p.y<h) d[p.xy]=s.Load(int3(p.xy,0));}";
                    ComPtr<ID3DBlob> code;
                    auto hr = D3DCompile(program, sizeof(program) - 1, "FrameDepthCopy", nullptr, nullptr,
                        "main", "cs_5_0", 0, 0, &code, nullptr);
                    if (FAILED(hr)) { return hr; }
                    hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_);
                    if (FAILED(hr)) { return hr; }
                    shaderDevice_ = device;
                }
                if (source_.Get() != source) {
                    ComPtr<ID3D11ShaderResourceView> srv;
                    const auto hr = device->CreateShaderResourceView(source, &view, &srv);
                    if (FAILED(hr)) { return hr; }
                    srv_ = std::move(srv); source_ = source;
                }
                if (destination_.Get() != destination) {
                    ComPtr<ID3D11UnorderedAccessView> uav;
                    const auto hr = device->CreateUnorderedAccessView(destination, nullptr, &uav);
                    if (FAILED(hr)) { return hr; }
                    uav_ = std::move(uav); destination_ = destination;
                }
                ComPtr<ID3D11ComputeShader> shader;
                ComPtr<ID3D11ShaderResourceView> srv;
                ComPtr<ID3D11UnorderedAccessView> uav;
                std::array<ID3D11ClassInstance*, 256> classes{}; UINT count = static_cast<UINT>(classes.size());
                context->CSGetShader(&shader, classes.data(), &count);
                context->CSGetShaderResources(0, 1, &srv); context->CSGetUnorderedAccessViews(0, 1, &uav);
                ID3D11ShaderResourceView* noSRV = nullptr; ID3D11UnorderedAccessView* noUAV = nullptr;
                context->CSSetShaderResources(0, 1, &noSRV); context->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
                context->CSSetShader(shader_.Get(), nullptr, 0);
                context->CSSetShaderResources(0, 1, srv_.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, uav_.GetAddressOf(), nullptr);
                context->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
                context->CSSetShaderResources(0, 1, &noSRV); context->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
                context->CSSetShader(shader.Get(), classes.data(), count);
                context->CSSetShaderResources(0, 1, srv.GetAddressOf()); context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                for (UINT i = 0; i < count; ++i) { if (classes[i]) { classes[i]->Release(); } }
                return S_OK;
            }

        private:
            ComPtr<ID3D11Texture2D> source_, destination_;
            ComPtr<ID3D11ShaderResourceView> srv_;
            ComPtr<ID3D11UnorderedAccessView> uav_;
            ComPtr<ID3D11ComputeShader> shader_;
            ComPtr<ID3D11Device> shaderDevice_;
        };
    }
}
