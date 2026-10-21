#include <PCH.h>
#include "FrameGen/NeuralRuntimeKernels.h"
#include "RendererSettingsController.h"
#include "RenderPipeline.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "PerformanceTuning.h"
#include "CommunityShaderIntegration.h"
#include "WeatherAppearanceRuntime.h"
#include "PluginPaths.h"
#include "NvidiaBaselinePolicy.h"

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
    settingsDraft.menuHotkey = upscaler_.mToggleOverlayHotkey;
    settingsDraft.upscaleType = upscaler_.mUpscaleType;
    settingsDraft.qualityLevel = upscaler_.mQualityLevel;
    settingsDraft.dlssPreset = upscaler_.mDLSSPreset;
    settingsDraft.autoExposure = upscaler_.mAutoExposure;
    settingsDraft.sharpening = upscaler_.mSharpening;
    settingsDraft.sharpness = upscaler_.mSharpness;
    settingsDraft.enableJitter = upscaler_.mEnableJitter;
    settingsDraft.nativeUI = upscaler_.mNativeUI;
    settingsDraft.reShadeBeforeUpscaling = upscaler_.mReShadeBeforeUpscaling;
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
    settingsDraft.appearance = Appearance::Runtime::Get().Configuration();
    // An NR request whose runtime is missing stays as saved: the switch can still
    // turn it off, its other rows are locked, validation keeps the unchanged
    // request and Apply gates execution. Masking it here lost it on any save.
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
                                                         bool a_saveAsDefault, const Overlay::Layout* layout)
{
    if (!settingsDraft.valid)
    {
        return RejectSettingsAction(a_saveAsDefault, "Settings draft is unavailable; reopen the menu.",
            [](const std::string& message) { logger::error("{}", message); });
    }
    std::string actionMessage;
    bool actionMessageIsError = false;
    const bool sourceUpscaler = host_.StartupConfigured();
    RendererSettingsCapabilities capabilities{sourceUpscaler, false, host_.DedicatedUITextureMode(), CommunityShaders::Active()};
    capabilities.nvidiaAdapter = SourceDLSSG::Backend::Get().NvidiaAdapter();
    capabilities.nvidiaFeatures = SourceDLSSG::Backend::Get().NvidiaFeaturesAvailable();
#if !defined(TRP_NO_NEURAL_RENDERING)
    // The draft may select a different runtime file; check the one it selects.
    const auto draftRuntime = TheosRenderPipeline::SourceDLSSG::ResolveNeuralRuntimeSetting(
        settingsDraft.sourceDLSSG.neuralRuntime, PluginPaths::Directory(),
        SourceDLSSG::Backend::Get().NeuralComputeCapability(), NeuralRendering::RuntimeKernelsSupported()).string();
    capabilities.neuralRuntime = TheosRenderPipeline::SourceDLSSG::NeuralRuntimePresent(draftRuntime);
    capabilities.neuralOperational = SourceDLSSG::Backend::Get().Ready() && !SourceDLSSG::Backend::Get().NeuralState().failed;
#endif
    const auto current = Capture(capabilities.neuralRuntime, false);
    if (const char* error = ValidateRendererSettings(settingsDraft, capabilities, &current))
    {
        return RejectSettingsAction(a_saveAsDefault, error,
            [](const std::string& message) { logger::error("{}", message); });
    }
    if (settingsDraft.textureProviderConnected &&
        !textures_.Apply(settingsDraft.textureProviderSettings, a_saveAsDefault))
    {
        return RejectSettingsAction(a_saveAsDefault, textures_.Status(),
            [](const std::string& message) { logger::error("{}", message); });
    }
    if (upscaler_.mToggleOverlayHotkey != settingsDraft.menuHotkey)
    {
        logger::info("[Overlay Input] menu key vk=0x{:02X} -> 0x{:02X}", upscaler_.mToggleOverlayHotkey,
                     settingsDraft.menuHotkey);
    }
    upscaler_.mToggleOverlayHotkey = settingsDraft.menuHotkey;
    upscaler_.mUpscaleType = settingsDraft.upscaleType;
    upscaler_.mQualityLevel = std::clamp(settingsDraft.qualityLevel, 0, 4);
    upscaler_.mDLSSPreset = settingsDraft.dlssPreset;
    upscaler_.mAutoExposure = settingsDraft.autoExposure;
    upscaler_.mSharpening = settingsDraft.sharpening;
    upscaler_.mSharpness = std::clamp(settingsDraft.sharpness, 0.0f, 1.0f);
    upscaler_.mEnableJitter = settingsDraft.enableJitter;
    upscaler_.mNativeUI = settingsDraft.nativeUI;
    upscaler_.mReShadeBeforeUpscaling = settingsDraft.reShadeBeforeUpscaling;
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
#if !defined(TRP_NO_NEURAL_RENDERING)
    // A changed runtime takes effect through NR's options below: the pass
    // recreates its feature on the new file at the next evaluation.
    if (frameGen_.settings.configuredRuntimePaths.neural != frameGen_.settings.sourceDLSSG.neuralRuntime) {
        logger::info("[SourceDLSSG NR] runtime {} -> {}", frameGen_.settings.configuredRuntimePaths.neural,
            frameGen_.settings.sourceDLSSG.neuralRuntime);
    }
    frameGen_.settings.configuredRuntimePaths.neural = frameGen_.settings.sourceDLSSG.neuralRuntime;
    frameGen_.settings.neuralRenderingRuntimePath = draftRuntime;
#endif
    if (host_.StartupConfigured())
    {
        auto& source = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        source.RequestProvider(frameGen_.settings.sourceDLSSG.provider);
        source.ConfigureVSync(frameGen_.settings.sourceDLSSG.vsync);
        source.ConfigureReflex(static_cast<sl::ReflexMode>(frameGen_.settings.sourceDLSSG.reflexMode));
        source.ConfigurePreset(frameGen_.settings.sourceDLSSG.preset);
        source.ConfigureXeFGFrameTime(frameGen_.settings.sourceDLSSG.xefgFrameTime);
        source.ConfigureXeFGSceneChangeThreshold(frameGen_.settings.sourceDLSSG.xefgSceneChangeThreshold);
        // A running XeFG presenter is recreated on the other SDK copy.
        source.ConfigureXeFGTensor(frameGen_.settings.sourceDLSSG.xefgTensorCores);
        source.ConfigureOutputFPSLimit(frameGen_.settings.sourceDLSSG.outputFPSLimit);
        source.ConfigureFrameLimit(frameGen_.settings.sourceDLSSG.frameLimit);
        source.ConfigureGeneration(frameGen_.settings.sourceDLSSG.generation);
        source.ConfigureXeFG(frameGen_.settings.sourceDLSSG.xefg);
        // The backend applies Enabled at the next present boundary.
        source.ConfigureHDROutput(frameGen_.settings.sourceDLSSG.hdrOutput);
        TheosRenderPipeline::SourceDLSSG::NeuralOptions options;
        options.enabled = frameGen_.settings.sourceDLSSG.neuralEnabled &&
            NeuralSettingsUnavailable(upscaler_.mUpscaleType, capabilities) == nullptr;
        options.runtimePath = frameGen_.settings.neuralRenderingRuntimePath;
        options.automaticRuntime =
            TheosRenderPipeline::SourceDLSSG::IsAutomaticNeuralRuntime(frameGen_.settings.configuredRuntimePaths.neural);
        options.tuning = frameGen_.settings.sourceDLSSG.neuralTuning;
        options.objectMasks = frameGen_.settings.sourceDLSSG.neuralObjectMasks;
        options.reconstruction = frameGen_.settings.sourceDLSSG.neuralReconstruction;
        options.extraPasses = frameGen_.settings.sourceDLSSG.neuralExtraPasses;
        options.beforeUpscaling = frameGen_.settings.sourceDLSSG.neuralBeforeUpscaling;
        options.passes = frameGen_.settings.sourceDLSSG.neuralPasses;
        options.combat = frameGen_.settings.sourceDLSSG.neuralCombat;
        source.ConfigureNeuralRendering(std::move(options));
    }
    Appearance::Runtime::Get().Configure(settingsDraft.appearance);
    if (a_saveAsDefault)
    {
        const bool saved = upscaler_.SaveINI(layout);
        actionMessage =
            saved ? "Saved to TheosRenderPipeline.User.ini; updates keep these settings." :
                    "Could not write TheosRenderPipeline.User.ini; settings remain active for this session.";
        actionMessageIsError = !saved;
    }
    else
    {
        actionMessage = "Session settings applied.";
        actionMessageIsError = false;
    }
    logger::info("[Overlay] source settings applied save={} mode={} quality={} preset={} requestedMode={} requestedQuality={} requestedPreset={}", a_saveAsDefault,
                 upscaler_.mUpscaleType, upscaler_.mQualityLevel, upscaler_.mDLSSPreset,
                 settingsDraft.upscaleType, settingsDraft.qualityLevel, settingsDraft.dlssPreset);
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
    if (!actionMessageIsError && settingsDraft.sourceDLSSG.neuralEnabled &&
        NeuralSettingsUnavailable(settingsDraft.upscaleType, capabilities)) {
        actionMessage += " NR remains unavailable; its saved request is unchanged.";
    }
    return {std::move(actionMessage), actionMessageIsError, true};
}

RendererSettingsResult RendererSettingsController::SetNeuralRenderingEnabled(bool enabled)
{
#if defined(TRP_NO_NEURAL_RENDERING)
    (void)enabled;
    return {"Neural Rendering is not included in this build.", true};
#else
    std::string actionMessage;
    bool actionMessageIsError = false;

    if (host_.StartupConfigured())
    {
        auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
        auto options = backend.NeuralConfiguration();
        options.runtimePath = SourceDLSSG::ResolveNeuralRuntimeSetting(frameGen_.settings.configuredRuntimePaths.neural,
            PluginPaths::Directory(), backend.NeuralComputeCapability(), NeuralRendering::RuntimeKernelsSupported());
        options.automaticRuntime = SourceDLSSG::IsAutomaticNeuralRuntime(frameGen_.settings.configuredRuntimePaths.neural);
        options.enabled = enabled;
        if (enabled && !TheosRenderPipeline::SourceDLSSG::NeuralRuntimePresent(options.runtimePath))
        {
            return {"No NR runtime is available for this selection and GPU. Install the matching runtime package or select another runtime.",
                    true};
        }
        if (options.enabled && (!backend.Ready() ||
            !SupportsNeuralRenderingMode(upscaler_.mUpscaleType, CommunityShaders::Active()) ||
            (!CommunityShaders::Active() && !host_.DedicatedUITextureMode())))
        {
            actionMessage = "Source NR requires DLSS or DLAA, dedicated UI Texture mode, and a configured NR runtime.";
            actionMessageIsError = true;
            return {actionMessage, actionMessageIsError};
        }
        frameGen_.settings.sourceDLSSG.neuralEnabled = options.enabled;
        backend.ConfigureNeuralRendering(std::move(options));
        actionMessage =
            !enabled ? "Neural Rendering disabled." : "Source NR requested for the next eligible world frame.";
        actionMessageIsError = false;
        return {actionMessage, actionMessageIsError, true};
    }
    actionMessage = "Source NVIDIA host is unavailable.";
    actionMessageIsError = true;
    return {actionMessage, actionMessageIsError};
#endif
}
} // namespace TheosRenderPipeline
