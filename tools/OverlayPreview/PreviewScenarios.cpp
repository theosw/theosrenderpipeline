#include "OverlayPreview.h"
#include "FrameGen/NeuralRuntimeFiles.h"
#include "FrameGen/NeuralAdmission.h"

#include <cmath>

// Representative menu states. The values are illustrative, not measurements;
// add a scenario when a change needs a state these do not cover.

using namespace TheosRenderPipeline;

namespace
{
constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;

// Game-facing Present intervals around `baseMs`, with a regular hitch.
std::vector<float> FrameTimes(float baseMs)
{
    std::vector<float> times;
    for (int i = 0; i < 120; ++i)
    {
        float time = baseMs + 0.35f * std::sin(i * 0.9f) + 0.2f * std::sin(i * 2.3f);
        if (i % 29 == 7)
            time += baseMs * 0.45f;
        times.push_back(time);
    }
    return times;
}

void SetTiming(PerformanceTuning::TimingSnapshot& timings, PerformanceTuning::D3D11Stage stage, float ms)
{
    const auto i = static_cast<std::size_t>(stage);
    timings.d3d11Ms[i] = ms;
    timings.d3d11Available[i] = true;
}

std::shared_ptr<const std::vector<Appearance::WeatherEntry>> Weathers()
{
    using Appearance::Group;
    const auto entry = [](std::uint32_t id, Group group, const char* name) {
        return Appearance::CatalogueEntry(id, {{"skyrim.esm", id & 0xFFFFFF}, group}, name);
    };
    return std::make_shared<const std::vector<Appearance::WeatherEntry>>(std::vector{
        entry(0x0012F89B, Group::Clear, "SkyrimClear"),
        entry(0x0010A7A8, Group::Clear, "SkyrimClearTU"),
        entry(0x0010A23C, Group::Cloudy, "SkyrimCloudyTU"),
        entry(0x0010E1F2, Group::Rain, "SkyrimStormRainTU"),
        entry(0x0010E3D4, Group::Snow, "SkyrimStormSnow"),
        entry(0x000C8221, Group::Exterior, "SkyrimFog"),
    });
}

// DLSS Quality at 1440p, NR before upscaling with two passes, native DLSS-G x4.
PreviewScenario Healthy()
{
    PreviewScenario scenario;
    scenario.name = "healthy";
    scenario.summary = "DLSS Quality, NR two passes before upscaling, DLSS-G x4";
    auto& view = scenario.view;
    view.nvidiaHostActive = view.hostStartupConfigured = view.upscalerReady = view.dedicatedUITexture = true;
    view.hostFrameGenerationEnabled = view.frameGenerationRequested = true;
    view.evaluationCount = 48213;
    view.outputWidth = 2560;
    view.outputHeight = 1440;
    view.renderWidth = 1708;
    view.renderHeight = 960;
    view.renderSizeX = 1708;
    view.renderSizeY = 960;
    view.displaySizeX = 2560;
    view.displaySizeY = 1440;
    view.upscaler.mode = DLSS;
    view.upscaler.quality = 2;
    view.refreshRate = 165.0;
    view.reShadeStatus = "ReShade is not loaded.";
    view.neuralRuntimePath = "Data\\SKSE\\Plugins\\TheosRenderPipeline\\nvngx_dlssnr.dll";
    view.textureProviderStatus = "TextureDownscaler not installed";
    view.memorySnapshot = {true, 16 * kGiB, 9 * kGiB + kGiB / 2, 15 * kGiB + kGiB / 4};

    view.neuralRuntimes = {std::string(SourceDLSSG::kAutomaticNeuralRuntime),
        std::string(SourceDLSSG::kDefaultNeuralRuntime), std::string(SourceDLSSG::kLatestNeuralRuntime),
        std::string(SourceDLSSG::kRTX40NeuralRuntime)};

    view.sourceReady = true;
    view.neuralObjectsAvailable = true;
    // As in game before a group is first switched on: supported, hook not yet installed.
    view.neuralObjectStatus = "Object capture starts when you switch a group on";
    view.generationActive = true;
    view.effectiveMultiplier = 4;
    view.uiRecomposition = true;
    auto& session = view.session;
    session.stage = SourceDLSSG::SessionStage::Rendering;
    session.operation = "present";
    session.stateQueries = 48000;
    session.state.status = sl::DLSSGStatus::eOk;
    session.state.numFramesToGenerateMax = 3;
    session.state.bIsDynamicMFGSupported = sl::eTrue;
    session.options.mode = sl::DLSSGMode::eOn;
    session.options.numFramesToGenerate = 3;
    session.options.enableUserInterfaceRecomposition = sl::eTrue;
    session.reflexRequested = session.reflexSubmitted = sl::ReflexMode::eLowLatency;
    session.uiRecompositionRequested = true;
    view.mfg.route = SourceDLSSG::MFGRoute::Native;
    view.mfg.status = "native NVIDIA multiplier";

    auto& applied = view.neuralApplied;
    applied.enabled = true;
    applied.beforeUpscaling = true;
    applied.passes = 2;
    applied.automaticRuntime = true;
    applied.runtimePath = std::filesystem::u8path(SourceDLSSG::kLatestNeuralRuntime);
    auto& neural = view.neuralState;
    neural.active = true;
    neural.effectivePasses = 2;
    neural.evaluations = 48000;
    neural.telemetry.gpuFrequency = 10'000'000;
    neural.telemetry.gpuSamples = 600;
    neural.telemetry.gpuTicksTotal = 600ull * 34'200;
    neural.telemetry.gpuTicksMaximum = 51'000;
    neural.status = "NR active";

    view.timingEnabled = true;
    auto& timings = view.timings;
    using Stage = PerformanceTuning::D3D11Stage;
    timings.lastCompletedGeneration = timings.lastCompletedFrameId = 1;
    timings.d3d11Samples = 4096;
    SetTiming(timings, Stage::kFrame, 11.82f);
    SetTiming(timings, Stage::kFrameGenInputs, 0.21f);
    SetTiming(timings, Stage::kInputColorCopy, 0.08f);
    SetTiming(timings, Stage::kMaskEncode, 0.05f);
    SetTiming(timings, Stage::kDLSS, 1.36f);
    SetTiming(timings, Stage::kRCAS, 0.12f);
    SetTiming(timings, Stage::kOutputCopy, 0.09f);
    SetTiming(timings, Stage::kHUDLessCopy, 0.11f);
    SetTiming(timings, Stage::kNativeUIComposition, 0.18f);
    SetTiming(timings, Stage::kPresentationCopy, 0.07f);
    using Stage12 = PerformanceTuning::D3D12Stage;
    const auto set12 = [&](Stage12 stage, float ms) {
        timings.d3d12Ms[static_cast<std::size_t>(stage)] = ms;
        timings.d3d12Available[static_cast<std::size_t>(stage)] = true;
    };
    set12(Stage12::kNeuralPrepare, 0.31f);
    set12(Stage12::kNeuralFinish, 0.44f);
    set12(Stage12::kOutput, 0.06f);
    set12(Stage12::kOutputList, 0.12f);
    set12(Stage12::kFrameGenTagList, 0.09f);
    set12(Stage12::kFrameGenerationHold, 2.84f);
    timings.frameGenerationHold = {2.81f, 3.37f, 4.02f, 1024};
    timings.holdExact = 1011;
    timings.holdBounded = 13;
    timings.gameFrameCadence = {16.9f, 19.4f, 24.8f, 1024};
    timings.d3d11Frame = {11.6f, 13.2f, 15.9f, 1024};
    timings.sourcePresentCpu = {0.42f, 0.91f, 1.63f, 1024};
    view.outputRate = {Telemetry::OutputSource::Streamline, 236.4f, true};

    view.appearance.context = {true, false, 13.4f, 1.0f, 0x1A26F, {}, {{"skyrim.esm", 0x0A7A8}, Appearance::Group::Clear}};
    view.weatherCatalogue = Weathers();

    auto& draft = scenario.applied;
    draft.valid = true;
    draft.upscaleType = DLSS;
    draft.qualityLevel = 2;
    draft.sourceDLSSG.neuralEnabled = true;
    draft.sourceDLSSG.neuralBeforeUpscaling = true;
    draft.sourceDLSSG.neuralPasses = 2;
    draft.sourceDLSSG.generation.generatedFrames = 3;
    scenario.draft = scenario.applied;
    scenario.frameTimesMs = FrameTimes(16.9f);
    scenario.renderedFps = 59.2f;
    return scenario;
}

PreviewScenario NROff()
{
    auto scenario = Healthy();
    scenario.name = "nr-off";
    scenario.summary = "DLAA with NR off and DLSS-G x2 on a two-frame runtime";
    auto& view = scenario.view;
    view.outputWidth = view.renderWidth = view.renderSizeX = view.displaySizeX = 1920;
    view.outputHeight = view.renderHeight = view.renderSizeY = view.displaySizeY = 1080;
    view.upscaler.mode = DLAA;
    view.refreshRate = 144.0;
    view.session.state.numFramesToGenerateMax = 1;
    view.session.state.bIsDynamicMFGSupported = sl::eFalse;
    view.session.options.numFramesToGenerate = 1;
    view.effectiveMultiplier = 2;
    view.neuralApplied = {};
    view.neuralState = {};
    view.outputRate.fps = 141.8f;
    view.memorySnapshot = {true, 12 * kGiB, 10 * kGiB + kGiB / 3, 11 * kGiB};
    auto& draft = scenario.applied;
    draft.upscaleType = DLAA;
    draft.sourceDLSSG.neuralEnabled = false;
    draft.sourceDLSSG.neuralPasses = 1;
    draft.sourceDLSSG.generation.generatedFrames = 1;
    scenario.draft = scenario.applied;
    scenario.frameTimesMs = FrameTimes(14.1f);
    scenario.renderedFps = 70.9f;
    return scenario;
}

// Two presets, the first active in the current cloudy weather, with unapplied edits.
PreviewScenario Presets()
{
    auto scenario = Healthy();
    scenario.name = "presets";
    scenario.summary = "Two weather presets, editing the active one with unapplied changes";
    auto& settings = scenario.applied.appearance;
    Appearance::Profile overcast;
    Appearance::SetChange(overcast, "Pass1Intensity", 1.2f);
    Appearance::SetChange(overcast, "Pass2Style", 7.0f);
    Appearance::SetChange(overcast, "Sharpness", 0.45f);
    const auto first = Appearance::AddPreset(settings, "Overcast warmth", overcast);
    Appearance::FindPreset(settings, first)->groups[static_cast<std::size_t>(Appearance::Group::Cloudy)] = true;
    Appearance::FindPreset(settings, first)->groups[static_cast<std::size_t>(Appearance::Group::Rain)] = true;
    Appearance::Profile interior;
    Appearance::SetChange(interior, "Pass1LocalTone", 0.8f);
    const auto second = Appearance::AddPreset(settings, "Interior clarity", interior);
    Appearance::FindPreset(settings, second)->groups[static_cast<std::size_t>(Appearance::Group::Interior)] = true;
    settings.enabled = Appearance::UsesPresets(settings);
    scenario.draft = scenario.applied;
    Appearance::SetChange(Appearance::FindPreset(scenario.draft.appearance, first)->profile, "Pass1LocalStructure", 1.35f);

    auto& view = scenario.view;
    view.appearance.context.incoming = {{"skyrim.esm", 0x0A23C}, Appearance::Group::Cloudy};
    view.appearance.result = Appearance::Evaluate(settings, view.appearance.context, {});
    scenario.selectedPreset = 0;
    return scenario;
}

PreviewScenario CommunityShaders()
{
    auto scenario = Healthy();
    scenario.name = "community-shaders";
    scenario.summary = "Community Shaders upscaling with TRP NR and DLSS-G x3";
    auto& view = scenario.view;
    view.communityShaders = true;
    view.session.options.numFramesToGenerate = 2;
    view.effectiveMultiplier = 3;
    view.outputRate.fps = 171.3f;
    scenario.applied.sourceDLSSG.generation.generatedFrames = 2;
    scenario.draft = scenario.applied;
    return scenario;
}

PreviewScenario HostUnavailable()
{
    auto scenario = Healthy();
    scenario.name = "host-unavailable";
    scenario.summary = "NVIDIA host failed at startup and the NR runtime is missing";
    auto& view = scenario.view;
    view.nvidiaHostActive = view.hostStartupConfigured = view.upscalerReady = false;
    view.hostFrameGenerationEnabled = false;
    view.evaluationCount = 0;
    view.renderWidth = view.renderHeight = view.outputWidth = view.outputHeight = 0;
    view.renderSizeX = view.displaySizeX;
    view.renderSizeY = view.displaySizeY;
    view.sourceReady = false;
    view.generationActive = false;
    view.effectiveMultiplier = 1;
    view.session = {};
    view.mfg = {};
    view.neuralApplied = {};
    view.neuralState = {};
    view.timings = {};
    view.outputRate = {};
    scenario.nrRuntimePresent = false;
    scenario.frameTimesMs = FrameTimes(22.4f);
    scenario.renderedFps = 44.6f;
    return scenario;
}

// Pass 2 keeps NPCs as Pass 1 left them and softens the player's tone.
PreviewScenario Objects()
{
    auto scenario = Healthy();
    scenario.name = "objects";
    scenario.summary = "Per-pass object rules: Pass 2 skips NPCs and softens the player";
    auto& masks = scenario.applied.sourceDLSSG.neuralObjectMasks;
    masks.passes[1].groups[2] = {true, 1.0f, 1.0f, 0.0f};
    masks.passes[1].groups[1] = {true, 0.6f, 1.0f, 0.8f};
    scenario.draft = scenario.applied;
    scenario.view.neuralObjectCurrent = true;
    return scenario;
}

// The widest pass strip: five tabs, Passes 2-5 paused by combat, Pass 3 and 5
// with their own settings and Pass 4 with an object rule.
PreviewScenario FivePasses()
{
    auto scenario = Healthy();
    scenario.name = "five-passes";
    scenario.summary = "Five NR passes paused to one in combat; Pass 3 and 5 independent";
    auto& nr = scenario.applied.sourceDLSSG;
    nr.neuralPasses = 5;
    nr.neuralCombat.inCombat = true;
    nr.neuralExtraPasses[1] = {false, 0.5f, 1, nr.neuralTuning};
    nr.neuralExtraPasses[1].tuning.intensity = 0.7f;
    nr.neuralExtraPasses[3] = {false, 0.75f, 0, nr.neuralTuning};
    nr.neuralExtraPasses[3].tuning.style = 2;
    nr.neuralObjectMasks.passes[3].groups[2] = {true, 1.0f, 1.0f, 0.5f};
    scenario.draft = scenario.applied;
    auto& view = scenario.view;
    view.neuralApplied.passes = 5;
    view.neuralApplied.extraPasses = nr.neuralExtraPasses;
    view.neuralState.effectivePasses = 1;
    view.neuralState.passOverride = TheosRenderPipeline::NeuralRendering::PassOverride::Combat;
    return scenario;
}

// NR after upscaling with the live comparison on: Split at 60% and a held frame.
PreviewScenario Compare()
{
    auto scenario = Healthy();
    scenario.name = "compare";
    scenario.summary = "NR after upscaling with Split at 60% and Freeze frame on";
    scenario.applied.sourceDLSSG.neuralBeforeUpscaling = false;
    scenario.draft = scenario.applied;
    auto& applied = scenario.view.neuralApplied;
    applied.beforeUpscaling = false;
    applied.compare = {true, 0.6f, true};
    return scenario;
}

// Ratio reconstruction of linear HDR input with adaptive exposure at +0.5 EV.
PreviewScenario Exposure()
{
    auto scenario = Healthy();
    scenario.name = "exposure";
    scenario.summary = "Ratio reconstruction, linear HDR input, adaptive exposure +0.5 EV";
    auto& reconstruction = scenario.applied.sourceDLSSG.neuralReconstruction;
    reconstruction.method = TheosRenderPipeline::NeuralRendering::ResolveMethod::Ratio;
    reconstruction.colorIsHDR = true;
    reconstruction.adaptiveExposure = true;
    reconstruction.exposureBias = 0.5f;
    scenario.draft = scenario.applied;
    scenario.view.neuralApplied.reconstruction = reconstruction;
    return scenario;
}

// Frame limit Steady, holding rendered frames at 58 FPS.
PreviewScenario Steady()
{
    auto scenario = Healthy();
    scenario.name = "steady";
    scenario.summary = "Frame limit Steady at 58 rendered FPS";
    scenario.applied.sourceDLSSG.frameLimit = TheosRenderPipeline::SourceDLSSG::FrameLimitMode::Steady;
    scenario.draft = scenario.applied;
    scenario.view.steadyRenderedFPS = 58.2f;
    return scenario;
}

// NR's model every other frame with Frame limit Off, so the Steady hint shows.
PreviewScenario ModelRate()
{
    auto scenario = Healthy();
    scenario.name = "model-rate";
    scenario.summary = "NR model every other frame, Frame limit Off";
    auto& reconstruction = scenario.applied.sourceDLSSG.neuralReconstruction;
    reconstruction.modelRate = TheosRenderPipeline::NeuralRendering::ModelRate::EveryOtherFrame;
    scenario.draft = scenario.applied;
    scenario.view.neuralApplied.reconstruction = reconstruction;
    scenario.view.neuralState.telemetry.modelRunShare = 0.5;
    scenario.view.neuralState.telemetry.skippedFrames = 24000;
    return scenario;
}

// Lab mode on Every other frame, so the Every third frame (Lab) switch shows.
PreviewScenario ModelRateLab()
{
    auto scenario = ModelRate();
    scenario.name = "model-rate-lab";
    scenario.summary = "NR model every other frame in Lab mode, with the Every third frame switch";
    scenario.labMode = true;
    return scenario;
}

// A non-NVIDIA GPU: XeSS upscaling and XeFG x2, with NR unavailable.
PreviewScenario Intel();
// Universal on NVIDIA: the tensor model was asked for x5 and safely refused, so
// the running presenter is x2 only and x3-x6 are greyed with Intel's reason.
PreviewScenario TensorRefused()
{
    auto scenario = Intel();
    scenario.name = "tensor-refused";
    scenario.summary = "XeFG tensor model refused at x5; x3-x6 greyed with the reason";
    auto& view = scenario.view;
    view.nvidiaAdapter = true;
    view.xefgUnlockAvailable = true;
    view.xefgTensorRequested = true;
    view.xefg.experimentalMFG = true;
    view.xefgUnlockStatus = "XeFG model safely refused; official 1.3.4 x2 retained: model catalog not recognised";
    view.xefgTensorStatus = view.xefgUnlockStatus;
    scenario.applied.sourceDLSSG.xefgTensorCores = true;
    scenario.applied.sourceDLSSG.xefg.generatedFrames = 4;
    scenario.applied.sourceDLSSG.xefg.experimentalMFG = true;
    scenario.draft = scenario.applied;
    return scenario;
}

PreviewScenario Intel()
{
    auto scenario = Healthy();
    scenario.name = "intel";
    scenario.summary = "XeSS Quality and XeFG x2 on a non-NVIDIA GPU; NR unavailable";
    auto& view = scenario.view;
    view.nvidiaAdapter = false;
    view.xessActive = true;
    view.upscaler.mode = XeSS;
    view.provider = view.requestedProvider = FrameGenerationProvider::XeFG;
    view.providerStatus = "XeFG presenting";
    view.effectiveMultiplier = 2;
    view.session = {};
    view.mfg = {};
    view.neuralApplied = {};
    view.neuralState = {};
    auto& xefg = view.xefg;
    xefg.enabled = xefg.prepared = xefg.uiTexture = true;
    xefg.framesPresented = 2;
    xefg.generatedPresents = 24100;
    xefg.frameTimeMs = 16.4f;
    xefg.maxGeneratedFrames = xefg.generatedFrames = 1;
    view.xefgUnlockStatus = "x3-x6 need the Universal edition";
    view.outputRate = {Telemetry::OutputSource::XeFG, 118.2f, true};
    auto& draft = scenario.applied;
    draft.upscaleType = XeSS;
    draft.sourceDLSSG.provider = FrameGenerationProvider::XeFG;
    draft.sourceDLSSG.neuralEnabled = false;
    draft.sourceDLSSG.xefg.generatedFrames = 1;
    scenario.draft = scenario.applied;
    scenario.frameTimesMs = FrameTimes(16.9f);
    return scenario;
}

PreviewScenario NRFriendly()
{
    auto scenario = Intel();
    scenario.name = "nr-friendly";
    scenario.summary = "Pascal research XeSS Quality reserves the NR extent while NR starts off";
    auto& view = scenario.view;
    view.nvidiaAdapter = true;
    view.nvidiaFeatures = view.nvidiaFrameGeneration = false;
    view.nrFriendlyRenderSizeActive = true;
    view.outputWidth = view.displaySizeX = 1280;
    view.outputHeight = view.displaySizeY = 720;
    view.renderWidth = view.renderSizeX = 768;
    view.renderHeight = view.renderSizeY = 432;
    view.neuralApplied.enabled = false;
    view.neuralApplied.beforeUpscaling = true;
    scenario.applied.sourceDLSSG.neuralEnabled = false;
    scenario.applied.sourceDLSSG.neuralBeforeUpscaling = true;
    scenario.draft = scenario.applied;
    return scenario;
}

PreviewScenario Pascal(const char* name, const char* reason)
{
    auto scenario = NRFriendly();
    scenario.name = name;
    scenario.summary = "Experimental Universal sm_61 NR admission";
    scenario.view.neuralPascal = true;
    scenario.view.neuralAdmissionReason = reason;
    scenario.view.neuralRuntimePath = "Data/SKSE/Plugins/TheosRenderPipeline/NVIDIA/nvngx_dlssnr_310.9.16.dll";
    scenario.view.neuralRuntimes = {"Automatic"};
    return scenario;
}

PreviewScenario Lab()
{
    auto scenario = Healthy();
    scenario.name = "lab";
    scenario.summary = "Lab mode with HDR output active on a 1000-nit display";
    scenario.labMode = true;
    auto& hdr = scenario.view.hdr;
    hdr.requested = hdr.native = hdr.display = hdr.displayKnown = true;
    hdr.displayMaxNits = 1015.0f;
    hdr.windowsSDRWhiteNits = 240.0f;
    hdr.reason = "HDR10 output active";
    scenario.applied.sourceDLSSG.hdrOutput.enabled = true;
    scenario.draft = scenario.applied;
    scenario.actionMessage = "Session settings applied.";
    return scenario;
}
} // namespace

std::vector<PreviewScenario> PreviewScenarios()
{
    return {Healthy(), NROff(), Presets(), CommunityShaders(), HostUnavailable(), Objects(), FivePasses(), Compare(), Exposure(), Steady(), ModelRate(), ModelRateLab(), Intel(), TensorRefused(), NRFriendly(),
        Pascal("pascal-universal", ""),
        Pascal("pascal-standard", NeuralRendering::kPascalEditionReason),
        Pascal("pascal-driver", NeuralRendering::kPascalDriverReason),
        Pascal("pascal-missing-pack", NeuralRendering::kPascalAssetReason),
        Pascal("pascal-low-budget", NeuralRendering::kPascalBudgetReason), Lab()};
}
