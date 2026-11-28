#include <PCH.h>
#include "Localization.h"
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "DLSSPreset.h"
#include "SessionDegradationText.h"
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

    if (!view.nvidiaHostActive || !view.upscalerReady)
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
    if (view.steppedAside) {
        view.pipelineHealth = UIHealth::kError;
        view.pipelineLabel = TR("Off this session");
    }

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
    if (view.upscaler.mode == XeSS || IsFSR(view.upscaler.mode)) { std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "%.0f%%", view.proxyScale * 100.0f); }
    std::snprintf(view.nativeDetail, sizeof(view.nativeDetail), "%d x %d", view.nativeWidth, view.nativeHeight);
    if (cs) {
        view.upscaleTitle = TR("CS upscaling");
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "%.0f%%", view.proxyScale * 100.0f);
    }
    if (view.steppedAside) {
        // The game renders and presents itself at its own size; TRP does not upscale.
        if (view.renderWidth == 0 || view.renderHeight == 0) {
            view.renderWidth = static_cast<uint32_t>(view.displaySizeX);
            view.renderHeight = static_cast<uint32_t>(view.displaySizeY);
        }
        std::snprintf(view.renderDetail, sizeof(view.renderDetail), "%d x %d", view.displaySizeX, view.displaySizeY);
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "%s", TR("Off this session"));
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
    // NVIDIA App and driver overrides never stop TRP; they colour the box they change.
    const auto& app = view.nvidiaApp;
    const char* undo = TR("To let TRP decide, open NVIDIA App > Graphics > Skyrim Special Edition and set it to "
                          "\"Use the 3D application setting\", then restart Skyrim.");
    view.upscaleAlert = view.generationAlert = view.neuralAlert = 0;
    view.upscaleAlertText.clear();
    view.generationAlertText.clear();
    view.neuralAlertText.clear();
    if (app.neuralModelOverride && view.neuralEnabled && view.sourceNeural.active)
    {
        view.neuralAlert = 1;
        view.neuralAlertText = TRF("NVIDIA App chooses the Neural Rendering model ({}). TRP's Network setting is not used.",
                                   app.neuralModel);
        view.neuralAlertText += std::string("\n") + undo;
    }
    if (app.upscalingModelOverride && view.nvidiaHostActive && !cs && view.upscaler.mode != XeSS && !IsFSR(view.upscaler.mode))
    {
        view.upscaleAlert = 1;
        view.upscaleAlertText = TRF("NVIDIA App chooses the DLSS model ({}). TRP's Model preset is not used.", app.upscalingModel);
        view.upscaleAlertText += std::string("\n") + undo;
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), TR("%.0f%% | Preset by NVIDIA App"), view.proxyScale * 100.0f);
    }
    const unsigned requestedMultiplier = view.session.options.numFramesToGenerate + 1;
    const bool trpGenerating = view.frameGenerationRuntimeActive;
    // With an override in place, what DLSS-G actually presents shows a forced
    // multiplier; Dynamic MFG varies it on purpose, so it is not compared.
    unsigned forcedMultiplier = app.forcedMultiplier;
    const unsigned presented = view.session.state.numFramesActuallyPresented;
    if (!forcedMultiplier && app.frameGenerationOverride && view.provider == TheosRenderPipeline::FrameGenerationProvider::NVIDIA &&
        view.session.options.mode != sl::DLSSGMode::eDynamic && presented > 1 && presented != requestedMultiplier)
    {
        forcedMultiplier = presented;
        view.nvidiaApp.forcedMultiplier = forcedMultiplier;
    }
    if (app.smoothMotion == 1 && trpGenerating)
    {
        view.generationAlert = 2;
        view.generationAlertText = TR("Two frame generators are on: NVIDIA Smooth Motion adds frames on top of TRP's, "
                                      "which causes stutter, smearing and extra input lag.\nTurn Smooth Motion off for "
                                      "Skyrim in NVIDIA App > Graphics, or turn TRP frame generation off.");
    }
    else if (app.smoothMotion == 1 && !view.steppedAside)
    {
        view.generationAlert = 1;
        std::snprintf(view.generationTitle, sizeof(view.generationTitle), "%s", TR("Smooth Motion (driver)"));
        view.generationAlertText = TR("NVIDIA Smooth Motion is generating frames instead of TRP. This works, but TRP's "
                                      "frame generation usually keeps menus and the HUD sharper.\nTo switch, turn Smooth "
                                      "Motion off in NVIDIA App and turn TRP frame generation on.");
    }
    else if (forcedMultiplier > 1 && trpGenerating && forcedMultiplier != requestedMultiplier)
    {
        view.generationAlert = 2;
        view.generationAlertText = TRF("NVIDIA App forces x{} for Skyrim, so TRP's Multiplier (x{}) is ignored.",
                                       forcedMultiplier, requestedMultiplier);
        view.generationAlertText += std::string("\n") + undo;
    }
    else if (app.frameGenerationOverride && trpGenerating)
    {
        view.generationAlert = 1;
        view.generationAlertText = TR("NVIDIA App overrides DLSS Frame Generation for Skyrim (model, version or mode). "
                                      "TRP's frame generation still runs with it.");
        view.generationAlertText += std::string("\n") + undo;
    }
    const bool conflict = view.upscaleAlert == 2 || view.generationAlert == 2 || view.neuralAlert == 2;
    if ((view.upscaleAlert | view.generationAlert | view.neuralAlert) != 0 && view.pipelineHealth == UIHealth::kHealthy)
    {
        view.pipelineLabel = conflict ? TR("NVIDIA App conflict") : TR("NVIDIA App override");
    }

    // Features TRP runs without this session: the same red box, with what is off,
    // that the game keeps running, and what to do. Leads any NVIDIA App text.
    namespace Degradation = TheosRenderPipeline::Degradation;
    view.sessionNoticeText.clear();
    view.sessionDegraded = view.loadingArtworkUnavailable = false;
    const auto lead = [](std::string& target, const std::string& text) {
        target = target.empty() ? text : text + "\n\n" + target;
    };
    for (const auto& entry : view.degraded)
    {
        if (!entry.active) { continue; }
        view.sessionDegraded = true;
        const auto text = Degradation::Explain(entry, view.degradedFacts,
            [](const char* english) { return TheosRenderPipeline::Localization::Translate(english); });
        const auto feature = Degradation::FeatureOf(entry.reason);
        if (feature == Degradation::Feature::FrameGeneration)
        {
            view.generationAlert = 2;
            lead(view.generationAlertText, text);
            // These two leave NVIDIA's own frame generation running; the others stop it.
            if (entry.reason != Degradation::Reason::MultiFrameGeneration && entry.reason != Degradation::Reason::AdapterUnidentified)
            {
                std::snprintf(view.generationTitle, sizeof(view.generationTitle), "%s", TR(Degradation::BoxLabel(entry.reason)));  // translated where shown
                // NR loses its inputs with it; the Neural Rendering tab points at the cause.
                if (view.neuralEnabled)
                {
                    view.neuralAlert = 2;
                    std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), "%s", TR(Degradation::BoxLabel(entry.reason)));  // translated where shown
                    lead(view.neuralAlertText, entry.reason == Degradation::Reason::GuideRefused ?
                        TR("Neural Rendering is paused together with frame generation. The Frame generation tab says why and what to do.") :
                        TR("Neural Rendering is off this session because frame generation could not start. "
                           "The Frame generation tab says why and what to do."));
                }
            }
        }
        else if (feature == Degradation::Feature::NeuralRendering)
        {
            view.neuralAlert = 2;
            lead(view.neuralAlertText, text);
        }
        else
        {
            view.loadingArtworkUnavailable |= feature == Degradation::Feature::LoadingArtwork;
            view.sessionNoticeText += (view.sessionNoticeText.empty() ? "" : "\n\n") + text;
        }
    }
    if (view.sessionDegraded && view.pipelineHealth == UIHealth::kHealthy)
    {
        view.pipelineLabel = TR("Reduced this session");
    }

    const auto& output = view.outputRate;
    view.outputText = output.available ? TRF("{:.1f} FPS", output.fps) : std::string(TR("unavailable"));
    view.outputLabel = TR("Runtime output");
    view.activeUpscaleStage = view.sourceDLSSGActive ? (view.sourceNeural.active ? "TRP DLSS NR" : "TRP DLSS")  // not translated: product names
                                                     : TR("NVIDIA host unavailable");
    if (view.upscaler.mode == XeSS) { view.activeUpscaleStage = view.sourceNeural.active ? "TRP XeSS + NR" : "TRP XeSS"; }  // not translated: product names
    if (IsFSR(view.upscaler.mode)) { view.activeUpscaleStage = view.sourceNeural.active ? "TRP FSR + NR" : "TRP FSR"; }
    if (cs) {
        view.activeUpscaleStage = view.sourceNeural.active ? TR("CS upscaling + TRP NR") : TR("CS upscaling");
    }
}
