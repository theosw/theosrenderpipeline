#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>

namespace TheosRenderPipeline
{
    // Optional direct composition. The caller retires GPU work before Reset.
    class NativeUIBlend
    {
    public:
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, DXGI_FORMAT format);
        bool Compose(ID3D11DeviceContext* context, ID3D11Texture2D* output,
            ID3D11ShaderResourceView* layer, UINT width, UINT height);
        void ResetAfterRetirement();
    private:
        struct Target {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
        };
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
        Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
        std::array<Target, 16> targets_;
        DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    };
}
