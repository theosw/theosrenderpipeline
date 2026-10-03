#pragma once

#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "FrameTrace.h"
#include "NvidiaUpscalerConfiguration.h"
#include "PerformanceTuning.h"
#include "VideoMemoryTelemetry.h"
#include "WeatherAppearanceController.h"
#include "FrameGen/SourceDLSSGHDROutput.h"
#include "FrameGen/SourceDLSSGMFG.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
#include "FrameGen/SourceDLSSGSession.h"

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
    bool upscalerNeedsRestart{}, upscalerFailed{}, upscalerChangeQueued{};
    int renderSizeX{}, renderSizeY{}, displaySizeX{}, displaySizeY{};
    bool nativeUI{true};
    bool reShadeBeforeUpscaling{};
    std::string reShadeStatus;
    int menuHotkey{0x23};
    bool frameGenerationRequested{};
    double refreshRate{};
    std::string neuralRuntimePath;
    bool sourceReady{};
    TheosRenderPipeline::SourceDLSSG::SessionSnapshot session{};
    TheosRenderPipeline::SourceDLSSG::MFGSnapshot mfg{};
    TheosRenderPipeline::SourceDLSSG::NeuralOptions neuralApplied{};
    TheosRenderPipeline::SourceDLSSG::NeuralSnapshot neuralState{};
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
