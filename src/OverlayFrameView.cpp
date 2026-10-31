#include <PCH.h>
#include "Localization.h"
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "DLSSPreset.h"
#include "UpscaleType.h"

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
    view.frameGenerationRuntimeActive = view.sourceDLSSGActive && view.generationActive;
    view.activeDisplayMultiplier = view.frameGenerationRuntimeActive && view.sourceDLSSGActive
                                       ? view.effectiveMultiplier
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
    view.pipelineLabel = view.pipelineHealth == UIHealth::kHealthy   ? TR("Healthy")
                         : view.pipelineHealth == UIHealth::kWarning ? TR("Warming up")
                         : view.pipelineHealth == UIHealth::kError   ? TR("Attention required")
                                                                     : TR("Bypassed");

    std::snprintf(view.renderDetail, sizeof(view.renderDetail), "%d x %d", view.renderSizeX, view.renderSizeY);
    view.upscaleTitle = ModeName(view.upscaler.mode);
    const char* presetShort = TheosRenderPipeline::DLSSPreset::ShortName(view.upscaler.preset);
    if (view.nvidiaHostActive)
    {
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), TR("%.0f%% | Preset %s"),
                      view.proxyScale * 100.0f, presetShort);
    }
    else
    {
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), TR("Unavailable | Preset %s"), presetShort);
    }
    if (view.upscaler.mode == XeSS) { std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "%.0f%%", view.proxyScale * 100.0f); }
    std::snprintf(view.nativeDetail, sizeof(view.nativeDetail), "%d x %d", view.nativeWidth, view.nativeHeight);
    if (cs) {
        view.upscaleTitle = TR("CS upscaling");
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "%.0f%%", view.proxyScale * 100.0f);
    }
    const auto& neural = view.neuralApplied;
    view.neuralEnabled = neural.enabled;
    view.neuralBeforeUpscaling = neural.beforeUpscaling;
    if (!neural.enabled || view.sourceNeural.failed)
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), "%s", view.sourceNeural.sessionDisabled ? TR("Off this session") : view.sourceNeural.failed ? TR("Failed") : TR("Off"));
    }
    else if (!view.sourceNeural.active)
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), TR("%s %s | waiting"),
                      neural.beforeUpscaling ? TR("Before") : TR("After"), cs ? "CS" : ModeName(view.upscaler.mode));
    }
    else
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), TR("%s %s | %d %s"),
                      neural.beforeUpscaling ? TR("Before") : TR("After"), cs ? "CS" : ModeName(view.upscaler.mode), view.sourceNeural.effectivePasses,
                      view.sourceNeural.effectivePasses == 1 ? TR("pass") : TR("passes"));
    }
    if (view.frameGenerationRuntimeActive)
    {
        std::snprintf(view.generationTitle, sizeof(view.generationTitle), "%s x%u", TheosRenderPipeline::ProviderName(view.provider), view.activeDisplayMultiplier);
    }
    else
    {
        std::snprintf(view.generationTitle, sizeof(view.generationTitle), "%s",
                      view.nvidiaHostActive ? (view.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG ? TR("XeFG off") : TR("DLSS-G off")) : TR("Generation unavailable"));
    }
    const auto& output = view.outputRate;
    view.outputText = output.available ? TRF("{:.1f} FPS", output.fps) : std::string(TR("unavailable"));
    view.outputLabel = TR("Runtime output");
    view.activeUpscaleStage = view.sourceDLSSGActive ? (view.sourceNeural.active ? "TRP DLSS NR" : "TRP DLSS")  // not translated: product names
                                                     : TR("NVIDIA host unavailable");
    if (view.upscaler.mode == XeSS) { view.activeUpscaleStage = view.sourceNeural.active ? "TRP XeSS + NR" : "TRP XeSS"; }  // not translated: product names
    if (cs) {
        view.activeUpscaleStage = view.sourceNeural.active ? TR("CS upscaling + TRP NR") : TR("CS upscaling");
    }
}
