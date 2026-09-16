#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace TheosRenderPipeline::Overlay
{
    // Borrow host views. A fallback view belongs only to this draw, so hiding
    // the overlay cannot retain an old texture across host retirement/resize.
    template<class Draw>
    HRESULT DrawWithRenderTarget(ID3D11Device* device, ID3D11Texture2D* texture,
        ID3D11RenderTargetView* borrowed, Draw&& draw)
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> temporary;
        if (!borrowed) {
            if (!device || !texture) { return E_INVALIDARG; }
            const auto result = device->CreateRenderTargetView(texture, nullptr, &temporary);
            if (FAILED(result)) { return result; }
            borrowed = temporary.Get();
        }
        draw(borrowed);
        return S_OK;
    }
}
