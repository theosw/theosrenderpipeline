#include "NvidiaHost.h"
#include <PCH.h>
#include "NeuralFrameEligibility.h"
#include "../DLSSBackend.h"
#include "../RenderPipeline.h"
#include "SourceFrameGeneration.h"
#include "SourceDLSSGBackend.h"
#include "SourceDLSSGCamera.h"
#include "FGInputDebugView.h"
#include "FGMotionRepair.h"
#include "SourceNvidiaFrameEvaluator.h"
#include "SourceGenerationPolicy.h"
#include "PerformanceTuning.h"
#include "NeuralCombatMode.h"
#include "WeatherAppearanceRuntime.h"

struct NvidiaHost::SourceNvidiaEvaluationOperations
{
    NvidiaHost& host;
    RenderPipeline& upscaler;
    TheosRenderPipeline::SourceDLSSG::NeuralOptions neuralOptions;
    sl::Constants constants{};
    sl::Constants preview{};
    bool previewValid{};
    ID3D11Texture2D* repairedMotion{};

    // Underwater frames carry no real motion; NR and generation get camera
    // motion instead. Repaired once, at whichever stage reads it first.
    template <class Frame>
    ID3D11Texture2D* Motion(const Frame& frame, const sl::Constants& camera)
    {
        if (repairedMotion) { return repairedMotion; }
        float clipToPrevClip[16];
        static_assert(sizeof(camera.clipToPrevClip) == sizeof(clipToPrevClip));
        std::memcpy(clipToPrevClip, &camera.clipToPrevClip, sizeof(clipToPrevClip));
        // The flag the engine uses for its own underwater look. Missing means
        // no repair: the worst case is 0.4.1's unrepaired underwater motion.
        const auto* water = RE::TESWaterSystem::GetSingleton();
        const bool underwater = water && water->playerUnderwater;
        auto& repair = TheosRenderPipeline::FGMotionRepair::Get();
        repairedMotion = repair.Apply(host.context_.Get(), frame.motion, frame.depth,
            clipToPrevClip, camera.depthInverted == sl::eTrue, frame.renderWidth, frame.renderHeight, underwater);
        LogUnderwater(underwater, repair.Enabled());
        LogRepair(repair);
        return repairedMotion;
    }

    // The repair is armed only underwater; log each change of the engine flag
    // (first 300) so a test shows whether it follows the camera.
    static void LogUnderwater(bool underwater, bool enabled)
    {
        static bool last{};
        static std::uint32_t changes{};
        if (underwater == last) { return; }
        last = underwater;
        if (++changes <= 300) {
            logger::info("[FGMotionRepair] player underwater={} repair {} changes={}", underwater,
                !enabled ? "off (Lab)" : underwater ? "armed" : "disarmed (engine motion passes through)", changes);
        }
    }

    // Every switch between engine and camera motion (first 300), and a summary
    // every 600 read frames, so a repair that flips on and off shows in the log.
    static void LogRepair(TheosRenderPipeline::FGMotionRepair& repair)
    {
        static bool repairing{};
        static std::uint32_t switches{}, window{}, repairedInWindow{};
        TheosRenderPipeline::FGMotionRepair::Decision decision;
        if (!repair.TakeDecision(decision)) { return; }
        constexpr auto samples = TheosRenderPipeline::FGMotionRepair::kSamples;
        if (decision.repaired != repairing && ++switches <= 300) {
            logger::info("[FGMotionRepair] frame={} {} disagree={}/{} varied={}/{} switches={}", decision.frame,
                decision.repaired ? "camera motion replaces engine motion" : "engine motion kept",
                decision.disagree, samples, decision.varied, samples, switches);
        }
        repairing = decision.repaired;
        repairedInWindow += decision.repaired;
        if (++window == 600) {
            logger::info("[FGMotionRepair] last 600 frames: repaired={} switches total={} last disagree={} varied={}",
                repairedInWindow, switches, decision.disagree, decision.varied);
            window = repairedInWindow = 0;
        }
    }

    bool NeuralEligible(const TheosRenderPipeline::SourceNvidiaFrameGuides& frame,
        const TheosRenderPipeline::SourceDLSSG::NeuralOptions& options) const
    {
        auto* ui = RE::UI::GetSingleton();
        const bool menu = ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
        // DLSS-G's UI and HUD-less guides are tagged only when the interface was
        // drawn. NR before upscaling reads just the world frame, so hiding the
        // interface (tm or the console) must not stop it with those guides.
        return TheosRenderPipeline::SourceDLSSG::NeuralFrameEligible(host.nativeUI_.Dedicated(),
            options.WorldOnly(), ui != nullptr, menu, frame.hudLessColor != nullptr, frame.uiColorAndAlpha != nullptr);
    }

    // Reads the camera without advancing its history. Returns whether it cut
    // since the last committed frame; NR and XeSS reuse the same constants.
    bool PreviewCamera(const TheosRenderPipeline::SourceNvidiaFrameGuides& frame)
    {
        previewValid = TheosRenderPipeline::SourceDLSSG::CaptureCameraConstants(upscaler.mGraphicsState,
            frame.renderWidth, frame.renderHeight, frame.jitterX, frame.jitterY,
            frame.reset, frame.jitterEnabled, preview, false);
        const bool cut = previewValid && preview.reset == sl::eTrue;
        if (cut && !frame.reset) {
            static std::uint32_t logged{};
            if (++logged <= 50 || logged % 100 == 0) {
                logger::info("[Transitions] camera cut resets the upscaler at evaluation {} (cuts={})", host.evaluationCount_, logged);
            }
        }
        return cut;
    }

    bool EvaluateNeuralBeforeDLSS(TheosRenderPipeline::SourceNvidiaFrameInputs& frame)
    {
#if !defined(TRP_NO_NEURAL_RENDERING)
        auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        auto& options = neuralOptions;
        const bool eligible = NeuralEligible(frame, options);
        TheosRenderPipeline::NeuralRendering::ApplyCombatMode(options, eligible);
        const bool cameraValid = options.enabled && options.beforeUpscaling && eligible && previewValid;
        return backend.EvaluateNeuralBeforeUpscaling(options, cameraValid ? &preview : nullptr,
            eligible, frame.input, cameraValid ? Motion(frame, preview) : frame.motion, frame.depth, {frame.renderWidth, frame.renderHeight}, frame.reset);
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
        const bool spatial = frame.spatialOnly || host.loadingScreenRoute_.Active(host.presentCount_);
        bool evaluated{};
        if (spatial) {
            host.loadingScreenResult_ = host.loadingScreenUpscaler_.Evaluate(host.context_.Get(), frame.input, frame.output);
            evaluated = SUCCEEDED(host.loadingScreenResult_);
            if (evaluated) {
                host.loadingScreenRoute_.SpatialSucceeded();
                if (!host.loadingScreenLogged_) {
                    logger::info("[SpatialUpscale] current color without temporal guides; native UI composition unchanged");
                    host.loadingScreenLogged_ = true;
                }
            }
        } else {
            if (host.XeSSActive()) {
                ScopedD3D11PerformanceStage timer{host.context_.Get(), PerformanceTuning::D3D11Stage::kDLSS};
                auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
                const auto hr = backend.XeSS().Evaluate(host.context_.Get(), frame.input, frame.motion, frame.depth,
                    frame.output, frame.jitterX, frame.jitterY, frame.reset, previewValid ? preview.depthInverted == sl::eTrue : true, frame.sharpness);
                evaluated = SUCCEEDED(hr);
                if (!evaluated) { logger::error("[XeSS] evaluation failed {} hr=0x{:08X}", backend.XeSS().Status(), static_cast<unsigned>(hr)); }
                if (evaluated && backend.XeSS().Frames() % 600 == 0) { logger::info("[XeSS] evaluatedFrames={} historyReset={} render={}x{}", backend.XeSS().Frames(), frame.reset, frame.renderWidth, frame.renderHeight); }
            } else evaluated = DLSSBackend::GetSingleton()->Evaluate(frame.input, frame.motion, frame.depth,
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
    void RenderReShade(const TheosRenderPipeline::SourceNvidiaFrameInputs& frame, bool before)
    {
        auto& effects = TheosRenderPipeline::ReShadeIntegration::Get();
        effects.SetBeforeUpscaling(upscaler.mReShadeBeforeUpscaling);
        auto* ui = RE::UI::GetSingleton();
        const bool world = ui && !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        const auto hr = effects.Render(before ? frame.input : frame.output, world ? frame.depth : nullptr,
            before ? TheosRenderPipeline::FrameExtent{frame.renderWidth, frame.renderHeight} :
                     TheosRenderPipeline::FrameExtent{frame.outputWidth, frame.outputHeight},
            {frame.renderWidth, frame.renderHeight}, before);
        if (FAILED(hr) && (effects.Snapshot().failures <= 3 || host.presentCount_ % 600 == 0)) {
            logger::warn("[ReShade] {} (0x{:08X})", effects.Status(), static_cast<unsigned>(hr));
        }
    }
    bool CaptureCamera(const TheosRenderPipeline::SourceNvidiaFrameGuides& frame)
    {
        return TheosRenderPipeline::SourceDLSSG::CaptureCameraConstants(upscaler.mGraphicsState,
            frame.renderWidth, frame.renderHeight, frame.jitterX, frame.jitterY,
            frame.reset, frame.jitterEnabled, constants);
    }
    bool Prepare(const TheosRenderPipeline::SourceNvidiaFrameGuides& frame)
    {
        ScopedD3D11PerformanceStage timer{host.context_.Get(), PerformanceTuning::D3D11Stage::kFrameGenInputs};
        auto* motion = Motion(frame, constants);
        if (TheosRenderPipeline::FGInputDebugView::Get().Enabled()) {
            float clipToPrevClip[16];
            std::memcpy(clipToPrevClip, &constants.clipToPrevClip, sizeof(clipToPrevClip));
            TheosRenderPipeline::FGInputDebugView::Get().Record(host.context_.Get(), motion, frame.depth, clipToPrevClip,
                constants.depthInverted == sl::eTrue, frame.renderWidth, frame.renderHeight);
        }
        return TheosRenderPipeline::SourceDLSSG::Backend::Get().Prepare(constants, motion, frame.depth,
            frame.uiColorAndAlpha, frame.hudLessColor, {frame.renderWidth, frame.renderHeight},
            frame.outputWidth, frame.outputHeight, NeuralEligible(frame, neuralOptions));
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
    const auto* guides = WorldGuides();
    frame.motion = guides ? guides->motion.Get() : nullptr;
    frame.depth = guides ? guides->depth.Get() : nullptr;
    frame.spatialOnly = !guides;
    // Unsampled, once per change, so a session stuck without guides is visible.
    if (frame.spatialOnly != guidelessLogged_ && guidelessChanges_ < 300) {
        guidelessLogged_ = frame.spatialOnly;
        ++guidelessChanges_;
        if (frame.spatialOnly) {
            logger::warn("[Guides] frame={} has no motion/depth from a completed world draw; scaled without DLSS, NR and frame generation (changes={})",
                presentCount_, guidelessChanges_);
        } else {
            logger::info("[Guides] frame={} has motion/depth again; DLSS resumes (changes={})", presentCount_, guidelessChanges_);
        }
    }
    // Stable tag identity is published now and filled after native UI drawing,
    // before Streamline consumes it at Present. Startup foreground is separate.
    frame.uiColorAndAlpha = nativeUIHandoff && nativeUI_.Available() ? nativeUI_.TaggedTexture() : nullptr;
    frame.hudLessColor = nativeUIHandoff ? gameTargets_.UpscaleOutput() : nullptr;
    frame.renderWidth = renderWidth_;
    frame.renderHeight = renderHeight_;
    frame.outputWidth = outputWidth_;
    frame.outputHeight = outputHeight_;
    auto neuralOptions = TheosRenderPipeline::SourceDLSSG::Backend::Get().NeuralConfiguration();
    bool sharpening = upscaler.mSharpening;
    float sharpness = upscaler.mSharpness;
    TheosRenderPipeline::Appearance::Runtime::Get().Apply(neuralOptions, sharpening, sharpness);
    frame.sharpness = sharpening ? sharpness : 0.0f;
    frame.jitterX = upscaler.mJitterOffsets[0] * jitterEnabled;
    frame.jitterY = upscaler.mJitterOffsets[1] * jitterEnabled;
    frame.motionScaleX = static_cast<float>(renderWidth_);
    frame.motionScaleY = static_cast<float>(renderHeight_);
    frame.reset = resetHistory || loadingScreenRoute_.NeedsTemporalReset();
    frame.jitterEnabled = upscaler.mEnableJitter;
    SourceNvidiaEvaluationOperations operations{*this, upscaler, std::move(neuralOptions)};
    loadingScreenResult_ = S_OK;
    const auto result = TheosRenderPipeline::SourceNvidiaFrameEvaluator::Evaluate(context_.Get(), frame, operations);
    if (!result.upscaled) {
        SetRuntimeEnabled(false);
        const auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        if (FAILED(loadingScreenResult_)) {
            status_ = std::format("Spatial scaling failed (0x{:08X}); generation held off",
                static_cast<std::uint32_t>(loadingScreenResult_));
        } else {
            status_ = !backend.Ready() ? backend.Status() :
                xeSSActive_ ? std::format("XeSS evaluation failed: {}; generation held off", backend.XeSS().Status()) :
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
