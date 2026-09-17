#include "NvidiaHost.h"
#include <PCH.h>
#include "../DLSSBackend.h"
#include "../RenderPipeline.h"
#include "SourceFrameGeneration.h"
#include "SourceDLSSGBackend.h"
#include "SourceDLSSGCamera.h"
#include "SourceNvidiaFrameEvaluator.h"
#include "SourceGenerationPolicy.h"
#include "PerformanceTuning.h"

struct NvidiaHost::SourceNvidiaEvaluationOperations
{
    NvidiaHost& host;
    RenderPipeline& upscaler;
    sl::Constants constants{};

    bool NeuralEligible(const TheosRenderPipeline::SourceNvidiaFrameGuides& frame) const
    {
        auto* ui = RE::UI::GetSingleton();
        return host.nativeUI_.Dedicated() && frame.uiColorAndAlpha && frame.hudLessColor && ui &&
            !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
    }

    bool EvaluateNeuralBeforeDLSS(TheosRenderPipeline::SourceNvidiaFrameInputs& frame)
    {
#if !defined(TRP_BASE_RENDERER)
        auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        const auto options = backend.NeuralConfiguration();
        sl::Constants preview{};
        const bool eligible = NeuralEligible(frame);
        const bool cameraValid = options.enabled && options.beforeUpscaling && eligible &&
            TheosRenderPipeline::SourceDLSSG::CaptureCameraConstants(upscaler.mGraphicsState,
                frame.renderWidth, frame.renderHeight, frame.jitterX, frame.jitterY,
                frame.reset, frame.jitterEnabled, preview, false);
        return backend.EvaluateNeuralBeforeUpscaling(options, cameraValid ? &preview : nullptr,
            eligible, frame.input, frame.motion, frame.depth, frame.reset);
#else
        (void)frame;
        return true;
#endif
    }

    void CopyInput(ID3D11DeviceContext* context, const TheosRenderPipeline::SourceNvidiaFrameInputs& frame)
    {
        ScopedD3D11PerformanceStage timer{context, PerformanceTuning::D3D11Stage::kInputColorCopy};
        context->CopyResource(frame.input, frame.color);
    }

    bool EvaluateDLSS(const TheosRenderPipeline::SourceNvidiaFrameInputs& frame)
    {
        const bool spatial = host.loadingScreenRoute_.Active(host.presentCount_);
        bool evaluated{};
        if (spatial) {
            host.loadingScreenResult_ = host.loadingScreenUpscaler_.Evaluate(host.context_.Get(), frame.input, frame.output);
            evaluated = SUCCEEDED(host.loadingScreenResult_);
            if (evaluated) {
                host.loadingScreenRoute_.SpatialSucceeded();
                if (!host.loadingScreenLogged_) {
                    logger::info("[LoadingScreen] spatial scaling active; native UI composition unchanged");
                    host.loadingScreenLogged_ = true;
                }
            }
        } else {
            evaluated = DLSSBackend::GetSingleton()->Evaluate(frame.input, frame.motion, frame.depth,
                frame.output, static_cast<int>(frame.renderWidth), static_cast<int>(frame.renderHeight),
                frame.sharpness, frame.jitterX, frame.jitterY, frame.motionScaleX, frame.motionScaleY,
                frame.reset);
            if (evaluated) {
                if (host.loadingScreenLogged_) {
                    logger::info("[LoadingScreen] resumed DLSS with temporal history reset");
                    host.loadingScreenLogged_ = false;
                }
                host.loadingScreenRoute_.TemporalSucceeded();
            }
        }
        return evaluated;
    }
    void UpscaleSucceeded() { ++host.upscaleEvaluationCount_; }
    bool CaptureCamera(const TheosRenderPipeline::SourceNvidiaFrameGuides& frame)
    {
        return TheosRenderPipeline::SourceDLSSG::CaptureCameraConstants(upscaler.mGraphicsState,
            frame.renderWidth, frame.renderHeight, frame.jitterX, frame.jitterY,
            frame.reset, frame.jitterEnabled, constants);
    }
    bool Prepare(const TheosRenderPipeline::SourceNvidiaFrameGuides& frame)
    {
        ScopedD3D11PerformanceStage timer{host.context_.Get(), PerformanceTuning::D3D11Stage::kFrameGenInputs};
        return TheosRenderPipeline::SourceDLSSG::Backend::Get().Prepare(constants, frame.motion, frame.depth,
            frame.uiColorAndAlpha, frame.hudLessColor, frame.outputWidth, frame.outputHeight, NeuralEligible(frame));
    }
    void PublishGeneration(bool prepared)
    {
        host.SetRuntimeEnabled(TheosRenderPipeline::SourceGenerationEnabled(host.warmupPresentsRemaining_,
            SourceFrameGeneration::GetSingleton()->RuntimeInterpolationRequested(), prepared,
            upscaler.FrameGenerationTransitionBlocked()));
    }
};

bool NvidiaHost::EvaluateSourceNvidiaFrame(bool nativeUIHandoff, bool resetHistory)
{
    auto& upscaler = *RenderPipeline::GetSingleton();
    const float jitterEnabled = upscaler.mEnableJitter ? 1.0f : 0.0f;
    TheosRenderPipeline::SourceNvidiaFrameInputs frame{};
    frame.color = gameTargets_.GameFacing();
    frame.input = gameTargets_.UpscaleInput();
    frame.output = gameTargets_.UpscaleOutput();
    frame.motion = upscaler.mMotionVectors.mImage;
    frame.depth = upscaler.mDepthBuffer.mImage;
    // Stable tag identity is published now and filled after native UI drawing,
    // before Streamline consumes it at Present. Startup foreground is separate.
    frame.uiColorAndAlpha = nativeUIHandoff && nativeUI_.Available() ? nativeUI_.TaggedTexture() : nullptr;
    frame.hudLessColor = nativeUIHandoff ? gameTargets_.UpscaleOutput() : nullptr;
    frame.renderWidth = renderWidth_;
    frame.renderHeight = renderHeight_;
    frame.outputWidth = outputWidth_;
    frame.outputHeight = outputHeight_;
    frame.sharpness = upscaler.mSharpening ? upscaler.mSharpness : 0.0f;
    frame.jitterX = upscaler.mJitterOffsets[0] * jitterEnabled;
    frame.jitterY = upscaler.mJitterOffsets[1] * jitterEnabled;
    frame.motionScaleX = static_cast<float>(renderWidth_);
    frame.motionScaleY = static_cast<float>(renderHeight_);
    frame.reset = resetHistory || loadingScreenRoute_.NeedsTemporalReset();
    frame.jitterEnabled = upscaler.mEnableJitter;
    SourceNvidiaEvaluationOperations operations{*this, upscaler};
    loadingScreenResult_ = S_OK;
    const auto result = TheosRenderPipeline::SourceNvidiaFrameEvaluator::Evaluate(context_.Get(), frame, operations);
    if (!result.upscaled) {
        SetRuntimeEnabled(false);
        const auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        if (FAILED(loadingScreenResult_)) {
            status_ = std::format("Loading-screen scaling failed (0x{:08X}); generation held off",
                static_cast<std::uint32_t>(loadingScreenResult_));
        } else {
            status_ = !backend.Ready() ? backend.Status() :
                std::format("TheosRenderPipeline direct DLSS evaluation failed (0x{:08X}); generation held off",
                    DLSSBackend::GetSingleton()->LastEvalResult());
        }
        if (!splitSourceRuntimeFailureLogged_) {
            splitSourceRuntimeFailureLogged_ = true;
            logger::error("[NvidiaHost] {}", status_);
        }
        return false;
    }
    if (!result.prepared && !splitSourceRuntimeFailureLogged_) {
        logger::warn("[SourceDLSSG] generation held off cameraValid={} backend={}", result.cameraValid,
            TheosRenderPipeline::SourceDLSSG::Backend::Get().Status());
    }
    splitSourceRuntimeFailureLogged_ = !result.prepared;
    return true;
}
