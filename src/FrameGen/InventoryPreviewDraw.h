#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <functional>
#include <vector>
#include "NativeUIAttachments.h"

namespace TheosRenderPipeline
{
    // Inventory3D's shaders were authored for drawing directly over the scene.
    // Their alpha is not necessarily foreground coverage. Keep RGB semantics,
    // and supply coverage only within the host's dedicated preview interval.
    class InventoryPreviewDraw
    {
    public:
        HRESULT Draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* nativeUI,
            const std::function<void()>& original, NativeUIDepthInputs depthInputs = {});
        void ResetAfterRetirement();
        bool Internal() const { return internal_; }
        UINT DepthSubstitutions() const { return depthSubstitutions_; }

    private:
        struct Blend
        {
            Microsoft::WRL::ComPtr<ID3D11BlendState> original, corrected;
            bool opaque{};
            HRESULT result{S_FALSE};
        };
        struct Depth
        {
            Microsoft::WRL::ComPtr<ID3D11DepthStencilState> original, readOnly;
            HRESULT result{S_FALSE};
        };
        Blend& ResolveBlend(ID3D11Device* device, ID3D11BlendState* original);
        Depth& ResolveDepth(ID3D11Device* device, ID3D11DepthStencilState* original);
        HRESULT EnsureCoverage(ID3D11Device* device, UINT width, UINT height);
        std::vector<Blend> blends_;
        std::vector<Depth> depths_;
        Microsoft::WRL::ComPtr<ID3D11BlendState1> coverage_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> mask_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> maskRTV_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskSRV_;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> stampVS_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> stampPS_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> stampBlend_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> stampDepth_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> stampRaster_;
        UINT width_{}, height_{};
        bool internal_{};
        UINT depthSubstitutions_{};
    };
}
