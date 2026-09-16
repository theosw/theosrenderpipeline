#pragma once

#include <d3d11.h>

namespace TheosRenderPipeline
{
void InstallPixelSamplerHook(ID3D11DeviceContext* deviceContext);
void InstallAdditionalSamplerHooks(ID3D11DeviceContext* deviceContext);
} // namespace TheosRenderPipeline
