#pragma once

#include <dxgi.h>
#include <d3d11.h>

void BeforeGameSwapChainPresent(IDXGISwapChain* a_swapChain);
void InstallUpscalerHooks();

void InstallUpscalerContextHooks(ID3D11Device* device, ID3D11DeviceContext* deviceContext);
