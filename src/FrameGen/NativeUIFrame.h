#pragma once

#include <d3d11.h>

namespace TheosRenderPipeline
{
    enum class BackgroundBoundary { World, Mist };

    // Frame decisions are independent of D3D bindings. In particular, an ENB
    // UI handoff can precede evaluation, which must then happen at Present.
    class NativeUIFrame
    {
    public:
        struct Action { bool evaluate{}, enterUI{}, clearDepth{}; };
        Action AfterBackground(BackgroundBoundary boundary, bool mainOrLoading, bool enbUI) const
        {
            if (presentPrepared_) { return {}; }
            if (enbUI) { return {false, !uiDrawn_, true}; }
            if (boundary == BackgroundBoundary::World && mainOrLoading) { return {}; }
            return {!evaluated_, !uiDrawn_, boundary == BackgroundBoundary::World};
        }
        void EvaluationSucceeded() { evaluated_ = true; }
        void EnteredUI() { uiDrawn_ = true; }
        void MistHandoffCompleted() { startupViewport_ = false; }
        void PreparingPresent() { presentPrepared_ = true; }
        bool Evaluated() const { return evaluated_; }
        bool UIDrawn() const { return uiDrawn_; }
        bool PresentPrepared() const { return presentPrepared_; }
        bool ReduceStartupViewport(bool mainOrLoading) const
        { return startupViewport_ && mainOrLoading && !presentPrepared_; }
        void NextFrame() { evaluated_ = uiDrawn_ = presentPrepared_ = false; }
        void NewSession() { NextFrame(); startupViewport_ = true; }
    private:
        bool evaluated_{}, uiDrawn_{}, presentPrepared_{};
        bool startupViewport_{true};
    };

    enum class ViewportChange { None, StartupRender, NativeUI };
    inline ViewportChange NativeUIViewport(const NativeUIFrame& frame, bool mainOrLoading,
        bool uiActive, bool internal, UINT count, const D3D11_VIEWPORT* input,
        UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight,
        D3D11_VIEWPORT& output)
    {
        // This conversion handles one viewport. Leave arrays and malformed
        // requests alone instead of forwarding a pointer to one stack element.
        if (internal || count != 1 || !input || !renderWidth || !renderHeight || !outputWidth || !outputHeight) {
            return ViewportChange::None;
        }
        output = *input;
        if (frame.ReduceStartupViewport(mainOrLoading)) {
            output.Width = static_cast<float>(renderWidth);
            output.Height = static_cast<float>(renderHeight);
            return ViewportChange::StartupRender;
        }
        if (uiActive && input->Width == static_cast<float>(renderWidth) && input->Height == static_cast<float>(renderHeight)) {
            output.Width = static_cast<float>(outputWidth);
            output.Height = static_cast<float>(outputHeight);
            return ViewportChange::NativeUI;
        }
        return ViewportChange::None;
    }
}
