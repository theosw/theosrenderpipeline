#pragma once

#include "SourceNvidiaFramePreparation.h"

namespace TheosRenderPipeline
{
    // Snapshot used by TRP's reconstruction stage. The host retains resource
    // ownership; per-frame NR reset changes stay local to the evaluation copy.
    struct SourceNvidiaFrameInputs : SourceNvidiaFrameGuides
    {
        ID3D11Texture2D* color{};
        ID3D11Texture2D* input{};
        ID3D11Texture2D* output{};
        float sharpness{}, motionScaleX{}, motionScaleY{};
    };

    struct SourceNvidiaFrameResult
    {
        bool upscaled{}, cameraValid{}, prepared{};
    };

    class SourceNvidiaFrameEvaluator
    {
    public:
        template<class Operations>
        static SourceNvidiaFrameResult Evaluate(ID3D11DeviceContext* context,
            SourceNvidiaFrameInputs frame, Operations& operations)
        {
            context->OMSetRenderTargets(0, nullptr, nullptr);
            operations.CopyInput(context, frame);
            // Completes the optional D3D12 NR round trip before D3D11 DLSS can
            // read input. Settings/re-entry resets apply to both stages.
            if (!operations.EvaluateNeuralBeforeDLSS(frame)) { return {}; }
            operations.RenderReShade(frame, true);
            if (!operations.EvaluateDLSS(frame)) { return {}; }
            operations.UpscaleSucceeded();
            // Finish external effects before Prepare snapshots HUD-less color.
            operations.RenderReShade(frame, false);

            const auto preparation = SourceNvidiaFramePreparation::PrepareCompletedFrame(frame, operations);
            return {true, preparation.cameraValid, preparation.prepared};
        }
    };
}
