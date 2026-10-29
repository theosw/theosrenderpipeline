#pragma once
#include <d3d11.h>

extern decltype(&ID3D11Device::CreateTexture2D) ptrCreateTexture2D;
HRESULT WINAPI hk_ID3D11Device_CreateTexture2D(ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** texture);

namespace TheosRenderPipeline
{
    void ObserveInventoryTextureCreation(const D3D11_TEXTURE2D_DESC* desc);
}
