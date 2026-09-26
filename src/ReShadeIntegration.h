#pragma once

#include "FrameGen/D3D11FrameCopy.h"
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace TheosRenderPipeline
{
    // One source-frame effect owner. The host's D3D12 presenter uses the public
    // native device handle; only the game-facing D3D11 stage hosts ReShade.
    class ReShadeIntegration final
    {
    public:
        static ReShadeIntegration& Get();
        void Discover(HWND window);
        HRESULT CreateSourceDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimum, ID3D12Device** out);
        void Configure(ID3D11Device* device, ID3D11DeviceContext* context, FrameExtent output);
        void SetBeforeUpscaling(bool before);
        HRESULT Render(ID3D11Texture2D* color, ID3D11Texture2D* depth,
            FrameExtent colorExtent, FrameExtent depthExtent, bool before);
        // Called at the host UI boundary even when TRP's own overlay is hidden.
        // Updates ReShade input/reloads and draws its GUI, with effects already
        // consumed or explicitly skipped. UI color is never an effect input.
        HRESULT FinishUI(ID3D11Texture2D* ui);
        void PresentCompleted();
        void ResetAfterRetirement();
        // The owned runtime's back buffer is the native UI layer, so ReShade's
        // own screenshot of it has no world. Each saved screenshot is queued
        // here for the presenter to replace with the final real frame.
        struct ScreenshotRequest { std::string path; int jpegQuality{90}; };
        bool TakeScreenshotRequest(ScreenshotRequest& request);
        bool Internal() const;
        bool OverlayOpen() const;
        const std::string& Status() const;
        struct Counters { std::uint64_t effects{}, updates{}, failures{}; };
        Counters Snapshot() const;
    private:
        ReShadeIntegration() = default;
        struct State;
        static State& Data();
    };
}
