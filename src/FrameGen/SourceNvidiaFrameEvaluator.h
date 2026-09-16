#pragma once

#include <d3d11.h>

namespace TheosRenderPipeline
{
    // Borrowed for this evaluation only. The host validates guide extents and
    // readiness, owns all resources, and consumes history only after copying the
    // completed output to the presentation buffer.
    struct SourceNvidiaFrameInputs
    {
        ID3D11Texture2D* color{};
        ID3D11Texture2D* input{};
        ID3D11Texture2D* output{};
        ID3D11Texture2D* motion{};
        ID3D11Texture2D* depth{};
        ID3D11Texture2D* uiColorAndAlpha{};
        ID3D11Texture2D* hudLessColor{};
        UINT renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
        float sharpness{}, jitterX{}, jitterY{}, motionScaleX{}, motionScaleY{};
        bool reset{}, jitterEnabled{};
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
            if (!operations.EvaluateDLSS(frame)) { return {}; }
            operations.UpscaleSucceeded();

            // Camera history must advance after successful DLSS only. Prepare
            // still runs with FG disabled: source NR uses these same inputs.
            const bool cameraValid = operations.CaptureCamera(frame);
            const bool prepared = cameraValid && operations.Prepare(frame);
            operations.PublishGeneration(prepared);
            return {true, cameraValid, prepared};
        }
    };
}
