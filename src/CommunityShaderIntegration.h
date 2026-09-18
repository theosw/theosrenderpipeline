#pragma once
#include <d3d11.h>
#include <dxgi.h>

namespace TheosRenderPipeline::CommunityShaders
{
    bool Active();
    void RememberEngineBoundary();
    void SelectRenderer();
    void InstallEngineHooks();
    void InstallDeviceHooks(ID3D11DeviceContext* context, IDXGISwapChain* swapChain);
}
