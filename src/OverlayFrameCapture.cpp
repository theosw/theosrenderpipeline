#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "RenderPipeline.h"
#include "CommunityShaderIntegration.h"
#include "ReShadeIntegration.h"
#include "WeatherAppearanceRuntime.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/DlssgLiteBlending.h"
#include "FrameGen/NeuralRuntimeFiles.h"
#include "FrameGen/NeuralRuntimeKernels.h"
#include "FrameGen/XeFGUnlock.h"
#include "PluginPaths.h"
#include "NeuralObjectHooks.h"

#include <chrono>

// The only place the menu reads the renderer. Drawing uses the returned view.
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
    view.textureProviderStatus = textureProvider->Status();
    videoMemory->Update();
    view.memorySnapshot = videoMemory->GetSnapshot();
    view.memoryStatus = videoMemory->Status();
    if (view.textureProviderAvailable && !settingsDraft.textureProviderConnected)
    {
        settingsDraft.textureProviderConnected = true;
        settingsDraft.textureProviderSettings = observedTextureSettings;
    }

    view.communityShaders = TheosRenderPipeline::CommunityShaders::Active();
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    view.nvidiaHostActive = nvidiaHost->ProxyActive();
    view.hostStartupConfigured = nvidiaHost->StartupConfigured();
    view.upscalerReady = nvidiaHost->UpscalerReady();
    view.dedicatedUITexture = nvidiaHost->DedicatedUITextureMode();
    view.hostFrameGenerationEnabled = nvidiaHost->FrameGenerationEnabled();
    view.evaluationCount = nvidiaHost->EvaluationCount();
    view.warmupPresentsRemaining = nvidiaHost->WarmupPresentsRemaining();
    view.renderWidth = nvidiaHost->RenderWidth();
    view.renderHeight = nvidiaHost->RenderHeight();
    view.outputWidth = nvidiaHost->OutputWidth();
    view.outputHeight = nvidiaHost->OutputHeight();
    view.xessActive = nvidiaHost->XeSSActive();
    view.nrFriendlyRenderSizeActive = nvidiaHost->NRFriendlyRenderSizeActive();
    const auto& upscalerSettings = nvidiaHost->SourceUpscalerSettings();
    view.upscaler = upscalerSettings.Effective();
    view.upscalerNeedsRestart = upscalerSettings.NeedsRestart();
    view.upscalerFailed = upscalerSettings.Failed();
    view.upscalerChangeQueued = upscalerSettings.NeedsLiveChange();
    view.upscalerLiveModeRejected = upscalerSettings.LiveModeRejected();
    view.upscalerStartupMode = upscalerSettings.Startup().mode;

    view.renderSizeX = upscaler->mRenderSizeX;
    view.renderSizeY = upscaler->mRenderSizeY;
    view.displaySizeX = upscaler->mDisplaySizeX;
    view.displaySizeY = upscaler->mDisplaySizeY;
    view.nativeUI = upscaler->mNativeUI;
    view.reShadeBeforeUpscaling = upscaler->mReShadeBeforeUpscaling;
    view.reShadeStatus = TheosRenderPipeline::ReShadeIntegration::Get().Status();
    view.menuHotkey = upscaler->mToggleOverlayHotkey;

    auto* frameGen = SourceFrameGeneration::GetSingleton();
    view.frameGenerationRequested = frameGen->RuntimeInterpolationRequested();
    view.refreshRate = frameGen->refreshRate;
    view.neuralRuntimePath = frameGen->settings.neuralRenderingRuntimePath;
#if !defined(TRP_NO_NEURAL_RENDERING)
    // Folder listings go through MO2's virtual file system; refresh every few seconds.
    using Clock = std::chrono::steady_clock;
    static std::vector<std::string> runtimes;
    static std::string listedFor;
    static Clock::time_point listed{};
    const auto& selectedRuntime = settingsDraft.sourceDLSSG.neuralRuntime;
    if (listedFor != selectedRuntime || Clock::now() - listed > std::chrono::seconds(3))
    {
        runtimes = TheosRenderPipeline::SourceDLSSG::ListNeuralRuntimes(TheosRenderPipeline::PluginPaths::Directory(),
                                                                        selectedRuntime);
        listedFor = selectedRuntime;
        listed = Clock::now();
    }
    view.neuralRuntimes = runtimes;
#endif

    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    view.sourceReady = backend.Ready();
#if !defined(TRP_NO_NEURAL_RENDERING)
    static TheosRenderPipeline::NeuralRendering::NeuralCapability admission;
    static Clock::time_point admittedAt{};
    static TheosRenderPipeline::SourceDLSSG::Preferences admittedPreferences;
    if (Clock::now() - admittedAt > std::chrono::seconds(3) ||
        admittedPreferences.neuralRuntime != settingsDraft.sourceDLSSG.neuralRuntime ||
        !TheosRenderPipeline::SameNeuralPreferences(admittedPreferences, settingsDraft.sourceDLSSG)) {
        auto options = backend.NeuralConfiguration();
        const auto& draft = settingsDraft.sourceDLSSG;
        options.runtimePath = TheosRenderPipeline::SourceDLSSG::ResolveNeuralRuntimeSetting(draft.neuralRuntime,
            TheosRenderPipeline::PluginPaths::Directory(), backend.NeuralComputeCapability(),
            TheosRenderPipeline::NeuralRendering::RuntimeKernelsSupported());
        options.passes = draft.neuralPasses;
        options.beforeUpscaling = draft.neuralBeforeUpscaling;
        options.reconstruction = draft.neuralReconstruction;
        options.extraPasses = draft.neuralExtraPasses;
        options = TheosRenderPipeline::SourceDLSSG::NormalizeNeuralRoute(std::move(options), view.communityShaders);
        admission = backend.CheckNeuralAdmission(options, options.beforeUpscaling ? view.renderWidth : view.outputWidth,
            options.beforeUpscaling ? view.renderHeight : view.outputHeight);
        admittedPreferences = draft; admittedAt = Clock::now();
    }
    view.neuralPascal = admission.Pascal();
    view.neuralAdmissionReason = admission.Refusal() ? admission.Refusal() : "";
#endif
    view.nvidiaAdapter = backend.NvidiaAdapter();
    view.nvidiaFeatures = backend.NvidiaFeaturesAvailable();
    view.nvidiaFrameGeneration = backend.NvidiaFrameGenerationAvailable();
    view.nvidiaServicesReady = backend.NvidiaServicesReady();
    view.generationActive = backend.GenerationActive();
    view.effectiveMultiplier = backend.EffectiveMultiplier();
    view.provider = backend.Provider();
    view.requestedProvider = backend.RequestedProvider();
    view.providerStatus = backend.ProviderStatus();
    view.nvidiaSwitchBlocked = backend.NvidiaSwitchBlocked();
    view.uiRecomposition = backend.UIRecompositionConfiguration();
    view.xefg = backend.XeFGState();
    view.xefgUnlockStatus = backend.XeFGUnlockStatus();
    view.xefgUnlockAvailable = TheosRenderPipeline::XeFGUnlock::Available();
    view.liteBlending = TheosRenderPipeline::SourceDLSSG::LiteBlending::State();
    const auto vsync = backend.VSyncState();
    view.vsyncObserved = vsync.observed;
    view.vsyncIncoming = vsync.submission.incomingSync;
    view.vsyncRequested = vsync.submission.sync;
    view.vsyncFailed = FAILED(vsync.result);
    view.vsyncRequiredOff = vsync.submission.autoRequiredOff;
    view.vsyncConfigured = backend.VSyncConfigurationMode();
    view.xefgTensorRequested = backend.XeFGTensorRequested();
    view.xefgTensorStatus = backend.XeFGTensorStatus();
    view.xefgPacingStatus = backend.XeFGPacingStatus();
    view.xefgFlipMetering = backend.XeFGFlipMetering();
    view.steadyRenderedFPS = backend.SteadyRenderedFPS();
    view.session = backend.Snapshot();
    view.mfg = backend.MFGState();
    view.neuralApplied = backend.NeuralConfiguration();
    view.neuralState = backend.NeuralState();
#if !defined(TRP_NO_NEURAL_RENDERING)
    view.neuralObjectStatus = TheosRenderPipeline::NeuralObjects::Status();
    view.neuralObjectCurrent = TheosRenderPipeline::NeuralObjects::Current() != nullptr;
    view.neuralObjectsAvailable = TheosRenderPipeline::NeuralObjects::Available();
#endif
    view.hdr = backend.HDRState();

    const auto* performance = PerformanceTuning::GetSingleton();
    view.timingEnabled = performance->TimingEnabled();
    view.timingQuarantined = performance->GetQueryDiagnostics().quarantined;
    view.timings = performance->GetTimingSnapshot();
    view.trace = FrameTrace::GetSingleton()->GetStatus();
    view.outputRate = outputRate.Rate();

    const auto& appearance = TheosRenderPipeline::Appearance::Runtime::Get();
    view.appearance = appearance.State();
    view.weatherCatalogue = appearance.Catalogue();

    DescribeFrameView(view);
    return view;
}
