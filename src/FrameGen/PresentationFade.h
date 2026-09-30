#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace TheosRenderPipeline
{
    // Scales a finished presentation frame towards black with a blend factor
    // (dest * factor). The device context state is swapped for the draw, so the
    // producer's bindings are untouched. Retire GPU work before Reset.
    class PresentationFade
    {
    public:
        bool Apply(ID3D11DeviceContext* context, ID3D11Texture2D* output, float factor);
        void ResetAfterRetirement();
    private:
        bool Initialize(ID3D11DeviceContext* context);
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
        Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
        bool failed_{};
    };
}
