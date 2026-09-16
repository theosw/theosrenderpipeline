#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include "NativeUIFrame.h"

namespace TheosRenderPipeline
{
    // Binding lifetime and frame decisions share an owner, but entering UI does
    // not imply evaluation. End must run before releasing retired resources.
    class NativeUIPass
    {
    public:
        struct Target
        {
            ID3D11RenderTargetView* color;
            ID3D11DepthStencilView* depth;
            UINT width, height;
            bool clearTransparent;
            bool clearDepth{true};
            bool setViewport{true};
        };

        NativeUIPass() = default;
        NativeUIPass(const NativeUIPass&) = delete;
        NativeUIPass& operator=(const NativeUIPass&) = delete;

        bool Active() const { return active_; }
        bool InternalBind() const { return internalBind_; }
        bool TargetBound() const { return targetBound_; }
        // Retained before evaluation/UI redirection; valid only for this pass.
        ID3D11DepthStencilView* BackgroundDepth() const { return active_ ? savedDSV_.Get() : nullptr; }
        void SetTargetBound(bool bound) { targetBound_ = bound; }
        bool HasEarlyEvaluation() const { return frame_.Evaluated(); }
        void ResetEvaluation() { frame_.NextFrame(); }
        NativeUIFrame& Frame() { return frame_; }
        const NativeUIFrame& Frame() const { return frame_; }

        void CaptureBindings(ID3D11DeviceContext* context)
        {
            if (!context || captured_) { return; }
            ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            context->OMGetRenderTargets(static_cast<UINT>(savedRTVs_.size()), views, savedDSV_.ReleaseAndGetAddressOf());
            for (size_t i = 0; i < savedRTVs_.size(); ++i) { savedRTVs_[i].Attach(views[i]); }
            savedViewportCount_ = static_cast<UINT>(savedViewports_.size());
            context->RSGetViewports(&savedViewportCount_, savedViewports_.data());
            captured_ = true;
        }

        bool Enter(ID3D11DeviceContext* context, const Target& target)
        {
            if (active_) { return true; }
            if (!context || !target.color) { return false; }
            CaptureBindings(context);
            internalBind_ = true;
            if (target.clearDepth && target.depth) {
                context->ClearDepthStencilView(target.depth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            }
            if (target.clearTransparent) {
                const float transparent[4]{};
                context->ClearRenderTargetView(target.color, transparent);
            }
            BindTarget(context, target);
            active_ = targetBound_ = true;
            frame_.EnteredUI();
            internalBind_ = false;
            return true;
        }

        // Evaluation after an earlier UI-only handoff may unbind the target.
        // Resume it without clearing the UI that has already been drawn.
        void BindTarget(ID3D11DeviceContext* context, const Target& target)
        {
            if (!context || !target.color) { return; }
            internalBind_ = true;
            context->OMSetRenderTargets(1, &target.color, target.depth);
            if (target.setViewport) {
                const D3D11_VIEWPORT viewport{0.0f, 0.0f,
                    static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f};
                context->RSSetViewports(1, &viewport);
            }
            targetBound_ = true;
            internalBind_ = false;
        }

        void End(ID3D11DeviceContext* context)
        {
            if (!captured_ || !context) {
                active_ = false;
                targetBound_ = false;
                return;
            }
            internalBind_ = true;
            ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            UINT count = 0;
            for (UINT i = 0; i < savedRTVs_.size(); ++i) {
                targets[i] = savedRTVs_[i].Get();
                if (targets[i]) { count = i + 1; }
            }
            context->OMSetRenderTargets(count, targets, savedDSV_.Get());
            context->RSSetViewports(savedViewportCount_, savedViewports_.data());
            internalBind_ = false;
            active_ = false;
            targetBound_ = false;
            DiscardSavedBindings();
        }

        template<class Finalize>
        bool FinishForPresent(ID3D11DeviceContext* context, Finalize&& finalize)
        {
            if (!frame_.Evaluated()) { return false; }
            End(context);
            finalize();
            frame_.NextFrame();
            return true;
        }

    private:
        void DiscardSavedBindings()
        {
            for (auto& view : savedRTVs_) { view.Reset(); }
            savedDSV_.Reset();
            savedViewportCount_ = 0;
            captured_ = false;
        }

        std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> savedRTVs_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> savedDSV_;
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> savedViewports_{};
        UINT savedViewportCount_{};
        bool active_{};
        bool internalBind_{};
        bool targetBound_{};
        bool captured_{};
        NativeUIFrame frame_;
    };
}
