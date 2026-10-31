#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace TheosRenderPipeline
{
    // Scales a finished presentation frame towards black with a blend factor
    // (dest * factor). Saves/restores producer bindings without state swaps,
    // preserving native dispatch hooks. Retire GPU work before Reset.
    class PresentationFade
    {
    public:
        bool Apply(ID3D11DeviceContext* context, ID3D11Texture2D* output, float factor);
        void ResetAfterRetirement();
    private:
        bool Initialize(ID3D11DeviceContext* context);
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
        bool failed_{};
    };
}
