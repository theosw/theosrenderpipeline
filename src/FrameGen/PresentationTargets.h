#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>

namespace TheosRenderPipeline
{
    // All host-held references to inner swapchain buffers live here. Selection
    // borrows from the cache. Reset requires completed GPU retirement and an
    // ended native UI pass; a failed retirement must retain this entire owner.
    class PresentationTargets
    {
    public:
        PresentationTargets() = default;
        PresentationTargets(const PresentationTargets&) = delete;
        PresentationTargets& operator=(const PresentationTargets&) = delete;
        const auto& Buffers() const { return buffers_; }
        ID3D11Texture2D* Texture() const { return selected_; }
        ID3D11RenderTargetView* RTV() const { return rtv_.Get(); }
        ID3D11DepthStencilView* DepthDSV() const { return depthDSV_.Get(); }
        void ResetAfterRetirement()
        {
            selected_ = nullptr;
            rtv_.Reset();
            depthDSV_.Reset();
            depth_.Reset();
            buffers_.clear();
        }
        HRESULT CacheAfterRetirement(IDXGISwapChain* swapchain)
        {
            ResetAfterRetirement();
            if (!swapchain) { return E_INVALIDARG; }
            DXGI_SWAP_CHAIN_DESC desc{};
            auto result = swapchain->GetDesc(&desc);
            if (FAILED(result)) { return result; }
            const auto count = std::max<UINT>(2, desc.BufferCount);
            buffers_.reserve(count);
            for (UINT index = 0; index < count; ++index) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
                result = swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer));
                if (FAILED(result) || !buffer) { ResetAfterRetirement(); return FAILED(result) ? result : E_FAIL; }
                buffers_.push_back(std::move(buffer));
            }
            return S_OK;
        }
        HRESULT Select(ID3D11Device* device, UINT index)
        {
            if (!device || index >= buffers_.size()) { return E_INVALIDARG; }
            auto* target = buffers_[index].Get();
            if (selected_ != target) { selected_ = target; rtv_.Reset(); }
            if (!rtv_) {
                const auto result = device->CreateRenderTargetView(target, nullptr, &rtv_);
                if (FAILED(result)) { selected_ = nullptr; return result; }
            }
            if (!depthDSV_) {
                D3D11_TEXTURE2D_DESC desc{};
                target->GetDesc(&desc);
                desc.MipLevels = 1; desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                desc.CPUAccessFlags = desc.MiscFlags = 0;
                auto result = device->CreateTexture2D(&desc, nullptr, &depth_);
                if (SUCCEEDED(result)) { result = device->CreateDepthStencilView(depth_.Get(), nullptr, &depthDSV_); }
                if (FAILED(result)) { depth_.Reset(); depthDSV_.Reset(); return result; }
            }
            return S_OK;
        }
    private:
        std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> buffers_;
        ID3D11Texture2D* selected_{};
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthDSV_;
    };
}
