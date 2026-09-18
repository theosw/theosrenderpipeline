#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "RenderPipeline.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/NvidiaHost.h"
#include "DLSSPreset.h"
#include "CommunityShaderIntegration.h"
#include "VideoMemoryTelemetry.h"
#include "FrameGen/SourceDLSSGBackend.h"

using namespace TheosRenderPipeline::Overlay;

OverlayUI::FrameView OverlayUI::CaptureFrameView()
{
    FrameView view;

    auto upscaler = RenderPipeline::GetSingleton();
    auto textureProvider = TextureProviderBridge::GetSingleton();
    auto videoMemory = VideoMemoryTelemetry::GetSingleton();
    if (!settingsDraft.valid)
    {
        CaptureSettingsDraft();
    }
    TextureProviderBridge::Settings observedTextureSettings{};
    view.textureProviderAvailable = textureProvider->Read(observedTextureSettings, &view.textureTelemetry);
    videoMemory->Update();
    view.memorySnapshot = videoMemory->GetSnapshot();
    if (view.textureProviderAvailable && !settingsDraft.textureProviderConnected)
    {
        settingsDraft.textureProviderConnected = true;
        settingsDraft.textureProviderSettings = observedTextureSettings;
    }

    view.avgMs = 0.0f;
    for (int i = 0; i < frameTimeCount; ++i)
    {
        view.avgMs += frameTimesMs[i];
    }
    view.avgMs = frameTimeCount > 0 ? view.avgMs / static_cast<float>(frameTimeCount) : 0.0f;

    auto* nvidiaHost = NvidiaHost::GetSingleton();
    view.nvidiaHostActive = nvidiaHost->ProxyActive();
    view.sourceDLSSGActive = view.nvidiaHostActive && nvidiaHost->StartupConfigured();
    const bool dlssgEnabled = view.sourceDLSSGActive && TheosRenderPipeline::SourceDLSSG::Backend::Get().Snapshot().GenerationActive();
    view.frameGenerationRuntimeActive = dlssgEnabled;
    view.activeDisplayMultiplier = view.frameGenerationRuntimeActive && view.sourceDLSSGActive
                                       ? TheosRenderPipeline::SourceDLSSG::Backend::Get().Snapshot().options.numFramesToGenerate + 1u
                                       : 1u;
    view.sourceNeural = view.sourceDLSSGActive ? TheosRenderPipeline::SourceDLSSG::Backend::Get().NeuralState()
                                               : TheosRenderPipeline::SourceDLSSG::NeuralSnapshot{};
    view.nativeWidth = view.nvidiaHostActive && nvidiaHost->OutputWidth() > 0
                           ? static_cast<int>(nvidiaHost->OutputWidth())
                           : upscaler->mDisplaySizeX;
    view.nativeHeight = view.nvidiaHostActive && nvidiaHost->OutputHeight() > 0
                            ? static_cast<int>(nvidiaHost->OutputHeight())
                            : upscaler->mDisplaySizeY;
    view.proxyScale = view.nvidiaHostActive && view.nativeWidth > 0
                          ? static_cast<float>(nvidiaHost->RenderWidth()) / view.nativeWidth
                          : 1.0f;

    view.upscaleHealth = UIHealth::kIdle;

    if (!view.nvidiaHostActive || !nvidiaHost->UpscalerReady() || view.sourceNeural.failed)
    {
        view.upscaleHealth = UIHealth::kError;
    }
    else if (nvidiaHost->EvaluationCount() == 0)
    {
        view.upscaleHealth = UIHealth::kWarning;
    }
    else
    {
        view.upscaleHealth = UIHealth::kHealthy;
    }

    view.worldHealth =
        upscaler->mRenderSizeX > 0 && upscaler->mRenderSizeY > 0 ? UIHealth::kHealthy : UIHealth::kWarning;
    view.nativeUIHealth =
        (TheosRenderPipeline::CommunityShaders::Active() || upscaler->mNativeUI) && view.nativeWidth > 0 && view.nativeHeight > 0 ? UIHealth::kHealthy : UIHealth::kIdle;
    view.presentHealth =
        view.nvidiaHostActive && nvidiaHost->EvaluationCount() > 0 ? UIHealth::kHealthy : UIHealth::kWarning;
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

    std::snprintf(view.renderDetail, sizeof(view.renderDetail), "%d x %d", upscaler->mRenderSizeX,
                  upscaler->mRenderSizeY);
    const auto& effective = nvidiaHost->SourceUpscalerSettings().Effective();
    view.upscaleTitle = ModeName(effective.mode);
    const char* presetShort = TheosRenderPipeline::DLSSPreset::ShortName(effective.preset);
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
    if (TheosRenderPipeline::CommunityShaders::Active()) {
        view.upscaleTitle = "CS upscaling";
        std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), "%.0f%%", view.proxyScale * 100.0f);
    }
    const auto neural = TheosRenderPipeline::SourceDLSSG::Backend::Get().NeuralConfiguration();
    view.neuralEnabled = neural.enabled;
    view.neuralBeforeUpscaling = neural.beforeUpscaling;
    if (!neural.enabled || view.sourceNeural.failed)
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), "%s", view.sourceNeural.failed ? "Failed" : "Off");
    }
    else if (!view.sourceNeural.active)
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), "%s %s | waiting",
                      neural.beforeUpscaling ? "Before" : "After", TheosRenderPipeline::CommunityShaders::Active() ? "CS" : "DLSS");
    }
    else
    {
        std::snprintf(view.neuralDetail, sizeof(view.neuralDetail), "%s %s | %d %s",
                      neural.beforeUpscaling ? "Before" : "After", TheosRenderPipeline::CommunityShaders::Active() ? "CS" : "DLSS", neural.passes,
                      neural.passes == 1 ? "pass" : "passes");
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
    const auto& output = outputRate.Rate();
    view.outputText = output.available ? std::format("{:.1f} FPS", output.fps) : std::string("unavailable");
    view.outputLabel = "Runtime output";
    view.activeUpscaleStage = view.sourceDLSSGActive ? (view.sourceNeural.active ? "TRP DLSS NR" : "TRP DLSS")
                                                     : "NVIDIA host unavailable";
    if (TheosRenderPipeline::CommunityShaders::Active()) {
        view.activeUpscaleStage = view.sourceNeural.active ? "CS upscaling + TRP NR" : "CS upscaling";
    }

    return view;
}

void OverlayUI::DrawPipelineSummary(const FrameView& view)
{
#if !defined(TRP_NO_NEURAL_RENDERING)
    const auto neuralTooltip = view.sourceNeural.status + "\nClick to open Neural Rendering settings.";
#endif
    PipelineDiagram diagram{
        {{{"World", view.renderDetail, "Game-rendered scene.\nClick to open Image settings.",
           SettingsPage::Image},
#if !defined(TRP_NO_NEURAL_RENDERING)
          {"Neural Rendering", view.neuralDetail, neuralTooltip.c_str(), SettingsPage::NeuralRendering,
           !view.neuralEnabled},
#endif
          {view.upscaleTitle, view.upscaleDetail,
           TheosRenderPipeline::CommunityShaders::Active() ? "Upscaling is controlled in the Community Shaders menu.\nClick to open Image status." :
           "DLSS reconstruction or native-resolution DLAA.\nClick to open Image settings.", SettingsPage::Image},
          {"Frame generation", view.generationTitle,
           "Adds generated frames between game-rendered frames.\nClick to open Frame generation settings.",
           SettingsPage::FrameGeneration, !view.frameGenerationRuntimeActive},
          {"Output", view.nativeDetail,
           "Final output resolution.\nClick to open Image settings.", SettingsPage::Image}}},
        view.nativeDetail,
        view.nativeUIHealth == UIHealth::kHealthy,
        view.pipelineLabel,
        HealthColor(view.pipelineHealth)};
#if !defined(TRP_NO_NEURAL_RENDERING)
    if (!view.neuralBeforeUpscaling)
    {
        std::swap(diagram.stages[1], diagram.stages[2]);
    }
#endif
    requestedPage = DrawPipelineDiagram(diagram);
}
