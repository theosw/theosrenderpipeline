#include "OverlayPreview.h"

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

    view.sourceReady = true;
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
    return {Healthy(), NROff(), Presets(), CommunityShaders(), HostUnavailable(), Lab()};
}
