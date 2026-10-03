#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "DLSSPreset.h"

using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DescribeFrameView(FrameView& view) const
{
    view.avgMs = 0.0f;
    for (int i = 0; i < frameTimeCount; ++i)
    {
        view.avgMs += frameTimesMs[i];
    }
    view.avgMs = frameTimeCount > 0 ? view.avgMs / static_cast<float>(frameTimeCount) : 0.0f;

    const bool cs = view.communityShaders;
    view.sourceDLSSGActive = view.nvidiaHostActive && view.hostStartupConfigured;
    view.frameGenerationRuntimeActive = view.sourceDLSSGActive && view.session.GenerationActive();
    view.activeDisplayMultiplier = view.frameGenerationRuntimeActive && view.sourceDLSSGActive
                                       ? view.session.options.numFramesToGenerate + 1u
                                       : 1u;
    view.sourceNeural = view.sourceDLSSGActive ? view.neuralState : TheosRenderPipeline::SourceDLSSG::NeuralSnapshot{};
    view.nativeWidth = view.nvidiaHostActive && view.outputWidth > 0 ? static_cast<int>(view.outputWidth)
                                                                     : view.displaySizeX;
    view.nativeHeight = view.nvidiaHostActive && view.outputHeight > 0 ? static_cast<int>(view.outputHeight)
                                                                       : view.displaySizeY;
    view.proxyScale = view.nvidiaHostActive && view.nativeWidth > 0
                          ? static_cast<float>(view.renderWidth) / view.nativeWidth
                          : 1.0f;

    view.upscaleHealth = UIHealth::kIdle;

    if (!view.nvidiaHostActive || !view.upscalerReady || view.sourceNeural.failed)
    {
        view.upscaleHealth = UIHealth::kError;
    }
    else if (view.evaluationCount == 0)
    {
        view.upscaleHealth = UIHealth::kWarning;
    }
    else
    {
        view.upscaleHealth = UIHealth::kHealthy;
    }

    view.worldHealth = view.renderSizeX > 0 && view.renderSizeY > 0 ? UIHealth::kHealthy : UIHealth::kWarning;
    view.nativeUIHealth =
        (cs || view.nativeUI) && view.nativeWidth > 0 && view.nativeHeight > 0 ? UIHealth::kHealthy : UIHealth::kIdle;
    view.presentHealth =
        view.nvidiaHostActive && view.evaluationCount > 0 ? UIHealth::kHealthy : UIHealth::kWarning;
    view.pipelineHealth = UIHealth::kHealthy;
    if (view.upscaleHealth == UIHealth::kError || view.presentHealth == UIHealth::kError)
    {
        view.pipelineHealth = UIHealth::kError;
    }
    else if (view.upscaleHealth == UIHealth::kWarning || view.worldHealth == UIHealth::kWarning)
    {
        view.pipelineHealth = UIHealth::kWarning;
    }
    view.pipelineLabel = view.pipelineHealth == UIHealth::kHealthy   ? "Healthy"
                         : view.pipelineHealth == UIHealth::kWarning ? "Warming up"
                         : view.pipelineHealth == UIHealth::kError   ? "Attention required"
                                                                     : "Bypassed";

    std::snprintf(view.renderDetail, sizeof(view.renderDetail), "%d x %d", view.renderSizeX, view.renderSizeY);
    view.upscaleTitle = ModeName(view.upscaler.mode);
    const char* presetShort = TheosRenderPipeline::DLSSPreset::ShortName(view.upscaler.preset);
    if (view.nvidiaHostActive)
    {
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "%.0f%% | Preset %s",
                      view.proxyScale * 100.0f, presetShort);
    }
    else
    {
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "Unavailable | Preset %s", presetShort);
    }
    std::snprintf(view.nativeDetail, sizeof(view.nativeDetail), "%d x %d", view.nativeWidth, view.nativeHeight);
    if (cs) {
        view.upscaleTitle = "CS upscaling";
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "%.0f%%", view.proxyScale * 100.0f);
    }
    const auto& neural = view.neuralApplied;
    view.neuralEnabled = neural.enabled;
    view.neuralBeforeUpscaling = neural.beforeUpscaling;
    if (!neural.enabled || view.sourceNeural.failed)
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), "%s", view.sourceNeural.failed ? "Failed" : "Off");
    }
    else if (!view.sourceNeural.active)
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), "%s %s | waiting",
                      neural.beforeUpscaling ? "Before" : "After", cs ? "CS" : "DLSS");
    }
    else
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), "%s %s | %d %s",
                      neural.beforeUpscaling ? "Before" : "After", cs ? "CS" : "DLSS", view.sourceNeural.effectivePasses,
                      view.sourceNeural.effectivePasses == 1 ? "pass" : "passes");
    }
    if (view.frameGenerationRuntimeActive)
    {
        std::snprintf(view.generationTitle, sizeof(view.generationTitle), "DLSS-G x%u", view.activeDisplayMultiplier);
    }
    else
    {
        std::snprintf(view.generationTitle, sizeof(view.generationTitle), "%s",
                      view.nvidiaHostActive ? "DLSS-G off" : "Generation unavailable");
    }
    const auto& output = view.outputRate;
    view.outputText = output.available ? std::format("{:.1f} FPS", output.fps) : std::string("unavailable");
    view.outputLabel = "Runtime output";
    view.activeUpscaleStage = view.sourceDLSSGActive ? (view.sourceNeural.active ? "TRP DLSS NR" : "TRP DLSS")
                                                     : "NVIDIA host unavailable";
    if (cs) {
        view.activeUpscaleStage = view.sourceNeural.active ? "CS upscaling + TRP NR" : "CS upscaling";
    }
}
