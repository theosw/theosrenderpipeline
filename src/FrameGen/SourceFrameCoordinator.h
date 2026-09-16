#pragma once

#include "NativeUIPass.h"

namespace TheosRenderPipeline
{
    // Source NVIDIA sequencing. The host supplies GPU/resource operations and
    // filters backend, swapchain and context eligibility before calling here.
    // NativeUIPass remains the single owner of bindings and frame decisions;
    // the host also uses it to end bindings after retirement and before release.
    class SourceFrameCoordinator
    {
    public:
        explicit SourceFrameCoordinator(NativeUIPass& pass) : pass_(pass) {}
        SourceFrameCoordinator(const SourceFrameCoordinator&) = delete;
        SourceFrameCoordinator& operator=(const SourceFrameCoordinator&) = delete;

        template<class Operations>
        void OnBackgroundReady(ID3D11DeviceContext* context, Operations& operations,
            BackgroundBoundary boundary, bool mainOrLoading)
        {
            // Observe the producer's target before preparation can change it.
            const bool enbUI = operations.ObserveENBBoundary();
            auto& frame = pass_.Frame();
            const auto action = frame.AfterBackground(boundary, mainOrLoading, enbUI);
            operations.Trace(boundary == BackgroundBoundary::World ? "world-complete" : "mist-complete");
            if (!operations.PrepareUITargets()) { return; }
            if (boundary == BackgroundBoundary::World && !enbUI) { operations.ClearAuxiliary(); }
            if (!action.evaluate && !action.enterUI) { return; }

            pass_.CaptureBindings(context);
            if (action.evaluate && !EvaluateBackground(operations, true)) {
                pass_.End(context);
                operations.Trace("boundary-evaluation-failed");
                return;
            }
            if (action.enterUI) {
                auto target = operations.UITarget();
                target.clearDepth = action.clearDepth;
                target.setViewport = !enbUI;
                if (!pass_.Enter(context, target)) { pass_.End(context); return; }
            }
            if (action.evaluate && !action.enterUI && pass_.Active()) {
                auto target = operations.UITarget();
                target.clearTransparent = false;
                pass_.BindTarget(context, target);
            }
            if (boundary == BackgroundBoundary::Mist && frame.UIDrawn()) { frame.MistHandoffCompleted(); }
            operations.Trace(enbUI ? "enb-ui-without-evaluation" : "native-ui-entered");
        }

        // The caller draws the overlay between preparation and completion.
        // End redirection before fallback evaluation, including UI-only ENB frames.
        template<class Operations>
        bool PrepareForPresent(ID3D11DeviceContext* context, Operations& operations)
        {
            operations.Trace("before-present");
            pass_.End(context);
            pass_.Frame().PreparingPresent();
            if (!pass_.HasEarlyEvaluation()) {
                if (!operations.PreparePresentTarget() || !EvaluateBackground(operations, pass_.Frame().UIDrawn())) {
                    operations.DisableRuntime();
                    operations.Trace("present-fallback-failed");
                    return false;
                }
                operations.Trace("present-fallback-evaluated");
            }
            return true;
        }

        template<class Operations>
        bool FinishForPresent(Operations& operations)
        {
            if (!pass_.Frame().PresentPrepared()) { return false; }
            if (pass_.HasEarlyEvaluation() && pass_.Frame().UIDrawn() && operations.NativeUIAvailable()) {
                if (!operations.ComposeNativeUI()) {
                    operations.DisableRuntime();
                    operations.CompositionFailed();
                }
            }
            // Even a failed fallback/composition consumes this frame. NextFrame
            // preserves the session's startup/Mist policy rather than restarting it.
            pass_.ResetEvaluation();
            return true;
        }

    private:
        template<class Operations>
        bool EvaluateBackground(Operations& operations, bool nativeUI)
        {
            if (pass_.HasEarlyEvaluation()) { return true; }
            if (!operations.EvaluateBackground(nativeUI)) { return false; }
            pass_.Frame().EvaluationSucceeded();
            return true;
        }

        NativeUIPass& pass_;
    };
}
