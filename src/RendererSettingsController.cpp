#include <PCH.h>
#include "RendererSettingsController.h"
#include "RenderPipeline.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "PerformanceTuning.h"
#include "CommunityShaderIntegration.h"

namespace TheosRenderPipeline
{
RendererSettingsController RendererSettingsController::Current()
{
    return {*RenderPipeline::GetSingleton(), *SourceFrameGeneration::GetSingleton(), *NvidiaHost::GetSingleton(),
            *PerformanceTuning::GetSingleton(), *TextureProviderBridge::GetSingleton()};
}

RendererSettingsDraft RendererSettingsController::Capture([[maybe_unused]] bool nrRuntimePresent, bool readTextures) const
{
    RendererSettingsDraft settingsDraft;
    settingsDraft.valid = true;
    settingsDraft.upscaleType = upscaler_.mUpscaleType;
    settingsDraft.qualityLevel = upscaler_.mQualityLevel;
    settingsDraft.dlssPreset = upscaler_.mDLSSPreset;
    settingsDraft.autoExposure = upscaler_.mAutoExposure;
    settingsDraft.sharpening = upscaler_.mSharpening;
    settingsDraft.sharpness = upscaler_.mSharpness;
    settingsDraft.enableJitter = upscaler_.mEnableJitter;
    settingsDraft.nativeUI = upscaler_.mNativeUI;
    settingsDraft.requestLoadingArtwork = upscaler_.mRequestLoadingArtwork.load(std::memory_order_relaxed);
    if (host_.StartupConfigured())
    {
        const auto& requested = host_.SourceUpscalerSettings().Requested();
        settingsDraft.upscaleType = requested.mode;
        settingsDraft.qualityLevel = requested.quality;
        settingsDraft.dlssPreset = requested.preset;
        settingsDraft.sharpening = requested.sharpening;
        settingsDraft.autoExposure = requested.autoExposure;
    }
    settingsDraft.lateOverlayBridge = upscaler_.mWheelerLateOverlayBridge;
    const auto& performanceSettings = performance_.settings;
    settingsDraft.enableGPUTimings = performanceSettings.enableGPUTimings;
    settingsDraft.enableFrameTrace = performanceSettings.enableFrameTrace;
    settingsDraft.directRCASOutput = performanceSettings.directRCASOutput;
    settingsDraft.directDLSSOutput = performanceSettings.directDLSSOutput;
    settingsDraft.sourceDLSSG = frameGen_.settings.sourceDLSSG;
#if !defined(TRP_BASE_RENDERER)
    // A saved NR request must not strand unrelated settings behind disabled controls.
    settingsDraft.sourceDLSSG.neuralEnabled &= nrRuntimePresent;
#endif
    settingsDraft.textureProviderConnected = readTextures && textures_.Read(settingsDraft.textureProviderSettings);
    return settingsDraft;
}

int RendererSettingsController::CountChanges(const RendererSettingsDraft& draft, bool nrRuntimePresent) const
{
    if (!draft.valid)
    {
        return 0;
    }
    auto current = Capture(nrRuntimePresent, draft.textureProviderConnected);
    if (host_.StartupConfigured())
    {
        const auto& requested = host_.SourceUpscalerSettings().Requested();
        current.upscaleType = requested.mode;
        current.qualityLevel = requested.quality;
        current.dlssPreset = requested.preset;
        current.autoExposure = requested.autoExposure;
        current.sharpening = requested.sharpening;
    }
    return CountRendererSettingsChanges(draft, current);
}

RendererSettingsResult RendererSettingsController::Apply(const RendererSettingsDraft& settingsDraft,
                                                         bool a_saveAsDefault)
{
    if (!settingsDraft.valid)
    {
        return {};
    }
    std::string actionMessage;
    bool actionMessageIsError = false;
    const bool sourceUpscaler = host_.StartupConfigured();
    RendererSettingsCapabilities capabilities{sourceUpscaler, false, host_.DedicatedUITextureMode(), CommunityShaders::Active()};
#if !defined(TRP_BASE_RENDERER)
    capabilities.neuralRuntime =
        TheosRenderPipeline::SourceDLSSG::NeuralRuntimePresent(frameGen_.settings.neuralRenderingRuntimePath);
#endif
    if (const char* error = ValidateRendererSettings(settingsDraft, capabilities))
    {
        return {error, true};
    }
    if (settingsDraft.textureProviderConnected &&
        !textures_.Apply(settingsDraft.textureProviderSettings, a_saveAsDefault))
    {
        actionMessage = textures_.Status();
        actionMessageIsError = true;
        logger::error("[Overlay] texture provider apply/save failed: {}", actionMessage);
        return {actionMessage, actionMessageIsError};
    }
    upscaler_.mUpscaleType = settingsDraft.upscaleType;
    upscaler_.mQualityLevel = std::clamp(settingsDraft.qualityLevel, 0, 4);
    upscaler_.mDLSSPreset = settingsDraft.dlssPreset;
    upscaler_.mAutoExposure = settingsDraft.autoExposure;
    upscaler_.mSharpening = settingsDraft.sharpening;
    upscaler_.mSharpness = std::clamp(settingsDraft.sharpness, 0.0f, 1.0f);
    upscaler_.mEnableJitter = settingsDraft.enableJitter;
    upscaler_.mNativeUI = settingsDraft.nativeUI;
    upscaler_.mRequestLoadingArtwork.store(settingsDraft.requestLoadingArtwork, std::memory_order_relaxed);
    upscaler_.mWheelerLateOverlayBridge = settingsDraft.lateOverlayBridge;
    // The host stages allocation changes and applies live changes after a completed Present.
    host_.RequestSourceUpscalerSettings({settingsDraft.upscaleType, settingsDraft.qualityLevel,
                                         settingsDraft.dlssPreset, settingsDraft.sharpening,
                                         settingsDraft.autoExposure});
    auto performanceSettings = performance_.settings;
    performanceSettings.enableGPUTimings = settingsDraft.enableGPUTimings;
    performanceSettings.enableFrameTrace = settingsDraft.enableFrameTrace;
    performanceSettings.directRCASOutput = settingsDraft.directRCASOutput;
    performanceSettings.directDLSSOutput = settingsDraft.directDLSSOutput;
    performance_.ApplySettings(performanceSettings);
    frameGen_.settings.sourceDLSSG = TheosRenderPipeline::SourceDLSSG::SanitizePreferences(settingsDraft.sourceDLSSG);
    if (host_.StartupConfigured())
    {
        auto& source = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        source.ConfigureReflex(static_cast<sl::ReflexMode>(frameGen_.settings.sourceDLSSG.reflexMode));
        source.ConfigureOutputFPSLimit(frameGen_.settings.sourceDLSSG.outputFPSLimit);
        source.ConfigureGeneration(frameGen_.settings.sourceDLSSG.generation);
        TheosRenderPipeline::SourceDLSSG::NeuralOptions options;
        options.enabled = frameGen_.settings.sourceDLSSG.neuralEnabled && (CommunityShaders::Active() || upscaler_.mUpscaleType == DLSS);
        options.runtimePath = frameGen_.settings.neuralRenderingRuntimePath;
        options.tuning = frameGen_.settings.sourceDLSSG.neuralTuning;
        options.reconstruction = frameGen_.settings.sourceDLSSG.neuralReconstruction;
        options.beforeUpscaling = frameGen_.settings.sourceDLSSG.neuralBeforeUpscaling;
        options.passes = frameGen_.settings.sourceDLSSG.neuralPasses;
        source.ConfigureNeuralRendering(std::move(options));
    }
    if (a_saveAsDefault)
    {
        const bool saved = upscaler_.SaveINI();
        actionMessage =
            saved ? "Startup defaults saved." : "Could not write TheosRenderPipeline.ini; settings remain active for this session.";
        actionMessageIsError = !saved;
    }
    else
    {
        actionMessage = "Session settings applied.";
        actionMessageIsError = false;
    }
    logger::info("[Overlay] source settings applied save={} mode={} quality={} preset={}", a_saveAsDefault,
                 upscaler_.mUpscaleType, upscaler_.mQualityLevel, upscaler_.mDLSSPreset);
    logger::info("[LoadingArtwork] request setting={} save={}", settingsDraft.requestLoadingArtwork, a_saveAsDefault);
    if (sourceUpscaler && !actionMessageIsError)
    {
        const auto& configuration = host_.SourceUpscalerSettings();
        if (configuration.Failed())
        {
            actionMessage = "Source DLSS configuration failed; restart required.";
            actionMessageIsError = true;
        }
        else if (configuration.NeedsRestart())
        {
            actionMessage =
                a_saveAsDefault
                    ? "Mode/quality saved for restart; live feature changes apply after this frame."
                    : "Mode/quality staged for restart; Save as default to keep them. Live feature changes are queued.";
        }
        else if (configuration.NeedsLiveChange())
        {
            actionMessage = a_saveAsDefault ? "Settings saved; feature update queued after this frame."
                                            : "Feature update queued after this frame; defaults unchanged.";
        }
        else
        {
            actionMessage = a_saveAsDefault ? "Settings saved." : "Session settings applied.";
        }
    }
    return {std::move(actionMessage), actionMessageIsError, true};
}

RendererSettingsResult RendererSettingsController::SetNeuralRenderingEnabled(bool enabled)
{
#if defined(TRP_BASE_RENDERER)
    (void)enabled;
    return {"Neural Rendering requires the optional NR + Ada MFG add-on.", true};
#else
    std::string actionMessage;
    bool actionMessageIsError = false;

    if (host_.StartupConfigured())
    {
        auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        auto options = backend.NeuralConfiguration();
        options.runtimePath = frameGen_.settings.neuralRenderingRuntimePath;
        options.enabled = enabled;
        if (enabled && !TheosRenderPipeline::SourceDLSSG::NeuralRuntimePresent(options.runtimePath))
        {
            return {"NR runtime DLL not found. Install nvngx_dlssnr.dll at the configured path and restart Skyrim.",
                    true};
        }
        if (options.enabled && (!backend.Ready() || (!CommunityShaders::Active() &&
            (upscaler_.mUpscaleType != DLSS || !host_.DedicatedUITextureMode()))))
        {
            actionMessage = "Source NR requires DLSS, dedicated UI Texture mode, and a configured NR runtime.";
            actionMessageIsError = true;
            return {actionMessage, actionMessageIsError};
        }
        frameGen_.settings.sourceDLSSG.neuralEnabled = options.enabled;
        backend.ConfigureNeuralRendering(std::move(options));
        actionMessage =
            !enabled ? "Standard DLSS requested." : "Source NR requested for the next eligible world frame.";
        actionMessageIsError = false;
        return {actionMessage, actionMessageIsError, true};
    }
    actionMessage = "Source NVIDIA host is unavailable.";
    actionMessageIsError = true;
    return {actionMessage, actionMessageIsError};
#endif
}
} // namespace TheosRenderPipeline
