#pragma once

#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "VideoMemoryTelemetry.h"
#include "FrameGen/SourceDLSSGNeuralState.h"

struct OverlayUI::FrameView
{
    TextureProviderBridge::Telemetry textureTelemetry{};
    bool textureProviderAvailable{};
    VideoMemoryTelemetry::Snapshot memorySnapshot{};
    float avgMs{};
    bool nvidiaHostActive{};
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
