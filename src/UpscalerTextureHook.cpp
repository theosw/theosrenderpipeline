// Allocation observation is deliberately independent of native guide ownership.
// The same hook is linked into the WARP fixture, including worker-thread calls.
#include "UpscalerTextureHook.h"

decltype(&ID3D11Device::CreateTexture2D) ptrCreateTexture2D;

HRESULT WINAPI hk_ID3D11Device_CreateTexture2D(ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** texture)
{
    const auto result = (device->*ptrCreateTexture2D)(desc, initial, texture);
    TheosRenderPipeline::ObserveInventoryTextureCreation(desc);
    return result;
}
