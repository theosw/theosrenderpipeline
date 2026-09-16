#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace TheosRenderPipeline
{
    // Owns the stable outer buffer and the upscaler handoff textures, never an
    // inner swapchain buffer. The host must retire GPU users before replacement
    // or release (or be in startup before submission). Failed retirement must
    // leave this owner untouched. Separate release operations preserve the
    // deferred-startup and output-only cleanup lifetimes.
    class GameFacingTargets
    {
    public:
        GameFacingTargets() = default;
        GameFacingTargets(const GameFacingTargets&) = delete;
        GameFacingTargets& operator=(const GameFacingTargets&) = delete;

        ID3D11Texture2D* GameFacing() const { return gameFacing_.Get(); }
        ID3D11Texture2D* UpscaleInput() const { return upscaleInput_.Get(); }
        ID3D11Texture2D* UpscaleOutput() const { return upscaleOutput_.Get(); }

        static D3D11_TEXTURE2D_DESC GameFacingDesc(
            D3D11_TEXTURE2D_DESC output, UINT renderWidth, UINT renderHeight)
        {
            // Preserve the complete inner contract, including shared metadata.
            output.Width = renderWidth;
            output.Height = renderHeight;
            output.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
            return output;
        }

        static D3D11_TEXTURE2D_DESC UpscaleInputDesc(
            const D3D11_TEXTURE2D_DESC& output, UINT renderWidth, UINT renderHeight)
        {
            auto input = GameFacingDesc(output, renderWidth, renderHeight);
            input.MipLevels = 1;
            input.ArraySize = 1;
            input.SampleDesc.Count = 1;
            input.SampleDesc.Quality = 0;
            input.Usage = D3D11_USAGE_DEFAULT;
            input.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            input.CPUAccessFlags = 0;
            input.MiscFlags = 0;
            return input;
        }

        static D3D11_TEXTURE2D_DESC UpscaleOutputDesc(const D3D11_TEXTURE2D_DESC& output)
        {
            // DLSS uses its own UAV output and copies to this plain SRV/RTV.
            return UpscaleInputDesc(output, output.Width, output.Height);
        }

        HRESULT CreateGameFacingAfterRetirement(ID3D11Device* device,
            const D3D11_TEXTURE2D_DESC& output, UINT renderWidth, UINT renderHeight)
        {
            if (!device) { return E_INVALIDARG; }
            const auto desc = GameFacingDesc(output, renderWidth, renderHeight);
            return device->CreateTexture2D(&desc, nullptr, gameFacing_.ReleaseAndGetAddressOf());
        }

        HRESULT CreateUpscaleInputAfterRetirement(ID3D11Device* device,
            const D3D11_TEXTURE2D_DESC& output, UINT renderWidth, UINT renderHeight)
        {
            if (!device) { return E_INVALIDARG; }
            const auto desc = UpscaleInputDesc(output, renderWidth, renderHeight);
            return device->CreateTexture2D(&desc, nullptr, upscaleInput_.ReleaseAndGetAddressOf());
        }

        HRESULT CreateUpscaleOutputAfterRetirement(ID3D11Device* device,
            const D3D11_TEXTURE2D_DESC& output)
        {
            if (!device) { return E_INVALIDARG; }
            const auto desc = UpscaleOutputDesc(output);
            return device->CreateTexture2D(&desc, nullptr, upscaleOutput_.ReleaseAndGetAddressOf());
        }

        void ResetGameFacingAfterRetirement() { gameFacing_.Reset(); }
        void ResetUpscaleOutputAfterRetirement() { upscaleOutput_.Reset(); }
        void ResetUpscalerAfterRetirement()
        {
            upscaleInput_.Reset();
            upscaleOutput_.Reset();
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11Texture2D> gameFacing_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> upscaleInput_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> upscaleOutput_;
    };
}
