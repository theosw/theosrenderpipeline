#pragma once

#include "D3D11ComputeBindings.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <format>
#include <string>
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

        // What a refused copy was given, for the log. The copies below return the
        // same E_INVALIDARG for a wrapped or foreign device, a format, a missing
        // binding and a size, and a player's log has to tell those apart.
        inline std::string Describe(ID3D11DeviceContext* context, ID3D11Texture2D* source,
            ID3D11Texture2D* destination, FrameExtent extent)
        {
            ComPtr<ID3D11Device> device;
            if (context) { context->GetDevice(&device); }
            const auto owner = [&](ID3D11Texture2D* texture) {
                if (!texture) { return "none"; }
                ComPtr<ID3D11Device> other; texture->GetDevice(&other);
                return SameObject(device.Get(), other.Get()) ? "same" : "differs";
            };
            const auto layout = [](ID3D11Texture2D* texture) -> std::string {
                if (!texture) { return "none"; }
                D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                return std::format("{}x{} format={} bind=0x{:X} mips={} array={} samples={}", desc.Width, desc.Height,
                    static_cast<unsigned>(desc.Format), desc.BindFlags, desc.MipLevels, desc.ArraySize, desc.SampleDesc.Count);
            };
            return std::format("context={} sourceDevice={} destinationDevice={} sameTexture={} extent={}x{} source=[{}] destination=[{}]",
                !context ? "none" : context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "deferred",
                owner(source), owner(destination), SameObject(source, destination), extent.width, extent.height,
                layout(source), layout(destination));
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
            // The check or D3D call that refused the last Copy. Each of them
            // returns before the context or either texture is touched.
            enum class Step { None, Resources, Layout, SourceFormat, ShaderDevice, ShaderCompile, ShaderCreate, SourceView, DestinationView };
            static constexpr const char* Name(Step step)
            {
                switch (step) {
                case Step::Resources: return "context, textures or their devices";
                case Step::Layout: return "sizes, destination format or bindings";
                case Step::SourceFormat: return "source depth format";
                case Step::ShaderDevice: return "device of the cached copy shader";
                case Step::ShaderCompile: return "copy shader compilation";
                case Step::ShaderCreate: return "copy shader creation";
                case Step::SourceView: return "source view creation";
                case Step::DestinationView: return "destination view creation";
                default: return "none";
                }
            }
            Step Refused() const { return refused_; }

            void ResetViews() { source_.Reset(); destination_.Reset(); srv_.Reset(); uav_.Reset(); }
            void ResetAfterRetirement() { ResetViews(); shader_.Reset(); shaderDevice_.Reset(); refused_ = Step::None; }

            HRESULT Copy(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                ID3D11Texture2D* destination, FrameExtent extent)
            {
                const auto refuse = [&](Step step, HRESULT result) { refused_ = step; return result; };
                if (!ValidResources(context, source, destination)) { return refuse(Step::Resources, E_INVALIDARG); }
                D3D11_TEXTURE2D_DESC from{}, to{}; source->GetDesc(&from); destination->GetDesc(&to);
                if (!extent.Fits(from) || !extent.Fits(to) || to.Width != extent.width || to.Height != extent.height ||
                    to.Format != DXGI_FORMAT_R32_FLOAT || !(from.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                    !(to.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) { return refuse(Step::Layout, E_INVALIDARG); }
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                switch (from.Format) {
                case DXGI_FORMAT_R24G8_TYPELESS: view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
                case DXGI_FORMAT_R32G8X24_TYPELESS: view.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
                case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: view.Format = DXGI_FORMAT_R32_FLOAT; break;
                default: return refuse(Step::SourceFormat, E_INVALIDARG);
                }
                view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
                ComPtr<ID3D11Device> device; context->GetDevice(&device);
                if (shader_) {
                    // ReShade can return its native device from a shader's
                    // GetDevice while contexts/resources expose the proxy.
                    // Retain the device that actually created our shader;
                    // never infer ownership from an incoming resource.
                    if (!SameObject(device.Get(), shaderDevice_.Get())) { return refuse(Step::ShaderDevice, E_INVALIDARG); }
                } else {
                    const auto& compiled = CompiledShader();
                    if (FAILED(compiled.hr) || !compiled.code) { return refuse(Step::ShaderCompile, compiled.hr); }
                    const auto hr = device->CreateComputeShader(compiled.code->GetBufferPointer(), compiled.code->GetBufferSize(), nullptr, &shader_);
                    if (FAILED(hr)) { return refuse(Step::ShaderCreate, hr); }
                    shaderDevice_ = device;
                }
                if (!SameObject(source_.Get(), source)) {
                    ComPtr<ID3D11ShaderResourceView> srv;
                    const auto hr = device->CreateShaderResourceView(source, &view, &srv);
                    if (FAILED(hr)) { return refuse(Step::SourceView, hr); }
                    srv_ = std::move(srv); source_ = source;
                }
                if (!SameObject(destination_.Get(), destination)) {
                    ComPtr<ID3D11UnorderedAccessView> uav;
                    const auto hr = device->CreateUnorderedAccessView(destination, nullptr, &uav);
                    if (FAILED(hr)) { return refuse(Step::DestinationView, hr); }
                    uav_ = std::move(uav); destination_ = destination;
                }
                refused_ = Step::None;
                {
                    D3D11ComputeBindings saved{context, 0, 1, 1};
                    ID3D11ShaderResourceView* noSRV = nullptr;
                    ID3D11UnorderedAccessView* noUAV = nullptr;
                    context->CSSetShaderResources(0, 1, &noSRV);
                    context->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
                    context->CSSetShader(shader_.Get(), nullptr, 0);
                    context->CSSetShaderResources(0, 1, srv_.GetAddressOf());
                    context->CSSetUnorderedAccessViews(0, 1, uav_.GetAddressOf(), nullptr);
                    context->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
                }
                return S_OK;
            }

        private:
            struct ShaderBytecode { ComPtr<ID3DBlob> code; HRESULT hr{E_FAIL}; };
            static const ShaderBytecode& CompiledShader()
            {
                static const ShaderBytecode cached = [] {
                    constexpr char program[] = "Texture2D<float> s:register(t0); RWTexture2D<float> d:register(u0); [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){uint w,h; d.GetDimensions(w,h); if(p.x<w && p.y<h) d[p.xy]=s.Load(int3(p.xy,0));}";
                    ShaderBytecode out;
                    out.hr = D3DCompile(program, sizeof(program) - 1, "FrameDepthCopy", nullptr, nullptr,
                        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out.code, nullptr);
                    if (FAILED(out.hr)) { out.code.Reset(); }
                    return out;
                }();
                return cached;
            }
            ComPtr<ID3D11Texture2D> source_, destination_;
            ComPtr<ID3D11ShaderResourceView> srv_;
            ComPtr<ID3D11UnorderedAccessView> uav_;
            ComPtr<ID3D11ComputeShader> shader_;
            ComPtr<ID3D11Device> shaderDevice_;
            Step refused_{};
        };
    }
}
