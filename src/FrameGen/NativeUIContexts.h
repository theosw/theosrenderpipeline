#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace TheosRenderPipeline
{
    // The inner swapchain can expose a different immediate-context interface
    // from the one returned to Skyrim by an outer D3D11/ENB wrapper. Register
    // that exact creation result, rather than admitting arbitrary contexts.
    class NativeUIContexts
    {
    public:
        bool RegisterGameContext(ID3D11DeviceContext* context)
        {
            if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return false; }
            gameContext_ = context;
            return true;
        }

        bool Contains(ID3D11DeviceContext* host, ID3D11DeviceContext* candidate) const
        {
            return host && candidate && (candidate == host || candidate == gameContext_.Get());
        }

        ID3D11DeviceContext* GameContext() const { return gameContext_.Get(); }
        void ResetAfterRetirement() { gameContext_.Reset(); }

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> gameContext_;
    };
}
