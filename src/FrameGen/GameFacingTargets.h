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

        // DLSS evaluates into its own UAV and copies here by default. The
        // opt-in direct-output routes instead bind this texture as the NGX or
        // RCAS destination, which removes a full-resolution copy per frame but
        // requires an unordered-access binding. Typed UAV support is optional
        // for some presentation formats, so the capability is requested rather
        // than assumed; without it this stays the proven plain SRV/RTV target.
        static D3D11_TEXTURE2D_DESC UpscaleOutputDesc(
            const D3D11_TEXTURE2D_DESC& output, bool unorderedAccess = false)
        {
            auto desc = UpscaleInputDesc(output, output.Width, output.Height);
            if (unorderedAccess) { desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS; }
            return desc;
        }

        static bool SupportsTypedUnorderedAccess(ID3D11Device* device, DXGI_FORMAT format)
        {
            UINT support = 0;
            return device && SUCCEEDED(device->CheckFormatSupport(format, &support)) &&
                (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
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

        // A device that reports the capability can still reject the allocation,
        // so a failed unordered-access attempt retries the proven descriptor
        // before reporting failure. The direct-output routes remain opt-in;
        // this only decides whether they can ever be eligible.
        HRESULT CreateUpscaleOutputAfterRetirement(ID3D11Device* device,
            const D3D11_TEXTURE2D_DESC& output)
        {
            if (!device) { return E_INVALIDARG; }
            if (SupportsTypedUnorderedAccess(device, output.Format)) {
                const auto capable = UpscaleOutputDesc(output, true);
                if (SUCCEEDED(device->CreateTexture2D(&capable, nullptr, upscaleOutput_.ReleaseAndGetAddressOf()))) {
                    return S_OK;
                }
            }
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
