#pragma once
#include "FrameGen/NeuralBackend.h"

#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "FrameTrace.h"
#include "NvidiaUpscalerConfiguration.h"
#include "PerformanceTuning.h"
#include "VideoMemoryTelemetry.h"
#include "WeatherAppearanceController.h"
#include "FrameGen/DlssgLiteBlending.h"
#include "FrameGen/FrameGenerationProvider.h"
#include "FrameGen/PresentationVSync.h"
#include "FrameGen/SourceDLSSGHDROutput.h"
#include "FrameGen/SourceDLSSGMFG.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
#include "FrameGen/SourceDLSSGSession.h"
#include "FrameGen/XeFGPresenter.h"

#include <memory>
#include <string>
#include <vector>

// Everything the menu shows about the renderer. CaptureFrameView copies it once
// per menu frame; DescribeFrameView derives the labels.
struct OverlayUI::FrameView
{
    // Captured renderer state.
    TextureProviderBridge::Telemetry textureTelemetry{};
    bool textureProviderAvailable{};
    std::string textureProviderStatus;
    VideoMemoryTelemetry::Snapshot memorySnapshot{};
    std::string memoryStatus;
    bool communityShaders{};
    bool nvidiaHostActive{};
    bool hostStartupConfigured{};
    bool upscalerReady{};
    bool dedicatedUITexture{};
    bool hostFrameGenerationEnabled{};
    std::uint64_t evaluationCount{};
    std::int32_t warmupPresentsRemaining{};
    unsigned renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    TheosRenderPipeline::Upscaler::Creation upscaler{};
    bool upscalerNeedsRestart{}, upscalerFailed{}, upscalerChangeQueued{}, upscalerLiveModeRejected{};
    int upscalerStartupMode{};
    int renderSizeX{}, renderSizeY{}, displaySizeX{}, displaySizeY{};
    bool nativeUI{true};
    bool reShadeBeforeUpscaling{};
    std::string reShadeStatus;
    int menuHotkey{0x23};
    bool frameGenerationRequested{};
    double refreshRate{};
    std::string neuralRuntimePath;
    // nvngx_dlssnr*.dll files NR can switch to, refreshed every few seconds.
    std::vector<std::string> neuralRuntimes;
    std::vector<std::string> neuralRuntimeReasons;
    // NR per frame last measured with each runtime this session, in ms; 0 if
    // not tried. Cleared when other NR settings change, so values compare.
    std::vector<float> neuralRuntimeMilliseconds;
    // Private test backends in the same list: the HIP port's NR per frame
    // (0 if not tried) and why it cannot run (empty if it can).
    float neuralHIPMilliseconds{}, neuralVulkanMilliseconds{};
    std::string neuralVulkanReason{TheosRenderPipeline::NeuralRendering::kVulkanNotChecked}, neuralVulkanPath;
    std::string neuralHIPReason{TheosRenderPipeline::NeuralRendering::kHIPGPUReason};
    int neuralComputeCapability{-1};
    bool neuralRuntimeKernels{};
    bool sourceReady{};
    std::string neuralAdmissionReason;
    bool neuralPascal{};
    bool nvidiaServicesReady{};
    bool nvidiaAdapter{true};
    bool amdAdapter{};
    std::string amdNeuralPrerequisite;
    bool nvidiaFeatures{true};
    bool nvidiaFrameGeneration{true};
    // NVIDIA App and driver settings for Skyrim that change what TRP asked for,
    // read from the driver profile and the loaded runtime files. Nothing here
    // stops TRP; the menu only says what is in effect.
    struct NvidiaAppState
    {
        bool profileRead{};                // Skyrim's driver profile was read
        bool upscalingModelOverride{};     // App chooses the DLSS model
        std::string upscalingModel;        // what the App selected, e.g. "Latest" or "M"
        bool frameGenerationOverride{};    // App replaces the DLSS-G model or version
        unsigned forcedMultiplier{};       // App forces this output multiplier; 0 when not forced
        bool neuralModelOverride{};        // App chooses the DLSS NR model
        std::string neuralModel;
        int smoothMotion{-1};              // driver Smooth Motion: -1 unknown, 0 off, 1 on
    } nvidiaApp{};
    bool videoMemoryFull{};
    bool xessActive{};
    bool nrFriendlyRenderSizeActive{};
    bool fsrActive{};
    bool fsrRuntimePresent{};
    std::string fsrRunning;      // what really runs, empty when FSR is not active
    std::string fsrFallback;     // why FSR 4 was not used, if it was requested
    const char* fsr4Reason{};    // nullptr when the FSR 4 button may be chosen
    bool fsr4NeedsRestart{};     // off Radeon: the force starts with the game
    bool fsrWatermarkActive{};
    bool generationActive{};
    unsigned effectiveMultiplier{1};
    TheosRenderPipeline::FrameGenerationProvider provider{}, requestedProvider{};
    std::string providerStatus;
    bool nvidiaSwitchBlocked{};
    bool uiRecomposition{};
    TheosRenderPipeline::XeFGSnapshot xefg{};
    std::string xefgUnlockStatus;
    bool xefgUnlockAvailable{};
    TheosRenderPipeline::SourceDLSSG::LiteBlending::Snapshot liteBlending{};
    // Presentation VSync: what the game asked for, what the presenter requested, and the applied mode.
    bool vsyncObserved{}, vsyncIncoming{}, vsyncRequested{}, vsyncFailed{}, vsyncRequiredOff{};
    TheosRenderPipeline::SourceDLSSG::VSyncMode vsyncConfigured{};
    bool xefgTensorRequested{}, xefgFlipMetering{};
    // Why applied XeFG even pacing is not pacing now; null while it can apply.
    const char* xefgEvenPacingInactive{};
    float steadyRenderedFPS{}; // Steady frame limit's current cap; 0 while measuring or off.
    bool steadyWithXeFG{};     // Lab: Steady allowed with XeFG.
    std::string xefgTensorStatus, xefgPacingStatus;
    TheosRenderPipeline::SourceDLSSG::SessionSnapshot session{};
    TheosRenderPipeline::SourceDLSSG::MFGSnapshot mfg{};
    TheosRenderPipeline::SourceDLSSG::NeuralOptions neuralApplied{};
    TheosRenderPipeline::SourceDLSSG::NeuralSnapshot neuralState{};
    std::string neuralObjectStatus;
    bool neuralObjectCurrent{};
    bool neuralObjectsAvailable{};
    TheosRenderPipeline::SourceDLSSG::HDROutputState hdr{};
    bool timingEnabled{}, timingQuarantined{};
    PerformanceTuning::TimingSnapshot timings{};
    FrameTrace::Status trace{};
    TheosRenderPipeline::Telemetry::OutputRate outputRate{};
    TheosRenderPipeline::Appearance::Snapshot appearance{};
    std::shared_ptr<const std::vector<TheosRenderPipeline::Appearance::WeatherEntry>> weatherCatalogue{
        std::make_shared<const std::vector<TheosRenderPipeline::Appearance::WeatherEntry>>()};

    // Derived by DescribeFrameView.
    float avgMs{};
    bool sourceDLSSGActive{};
    bool frameGenerationRuntimeActive{};
    unsigned activeDisplayMultiplier{};
    TheosRenderPipeline::SourceDLSSG::NeuralSnapshot sourceNeural{};
    int nativeWidth{};
    int nativeHeight{};
    float proxyScale{};
    TheosRenderPipeline::Overlay::UIHealth upscaleHealth{};
    TheosRenderPipeline::Overlay::UIHealth worldHealth{};
    TheosRenderPipeline::Overlay::UIHealth nativeUIHealth{};
    TheosRenderPipeline::Overlay::UIHealth presentHealth{};
    TheosRenderPipeline::Overlay::UIHealth pipelineHealth{};
    const char* pipelineLabel{};
    // Pipeline boxes the NVIDIA App or driver changes: 0 none, 1 override in
    // effect (fine), 2 conflict the user should resolve. The text explains it.
    int upscaleAlert{}, generationAlert{}, neuralAlert{};
    std::string upscaleAlertText, generationAlertText, neuralAlertText;
    char renderDetail[64]{};
    char upscaleDetail[96]{};
    char nativeDetail[64]{};
    char generationTitle[48]{};
    const char* upscaleTitle{};
    bool neuralEnabled{};
    bool neuralBeforeUpscaling{true};
    char neuralDetail[96]{};
    std::string outputText{};
    const char* outputLabel{};
    const char* activeUpscaleStage{};
};
