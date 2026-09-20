#include "RendererSettings.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
#include "FrameGen/SourceDLSSGNeuralAvailability.h"
#include <cstdio>
#include <cstdlib>

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

int main()
{
    using namespace TheosRenderPipeline;
    using namespace TheosRenderPipeline::SourceDLSSG;
    NeuralOptions native;
    Require(!native.WorldOnly(), "native after-upscale placement retains UI composition");
    native.beforeUpscaling = true;
    Require(native.WorldOnly(), "native before-upscale placement excludes UI");
    NeuralOptions external;
    external.enabled = true; external.worldOnly = true;
    Require(external.WorldOnly() && !external.beforeUpscaling, "external after-upscale placement excludes UI without relabelling placement");
    NeuralHistory history;
    Require(history.ResetFor(external, true, false), "first eligible external frame resets history");
    Require(!history.ResetFor(external, true, false), "continuous external frames retain history");
    external.beforeUpscaling = true;
    Require(history.ResetFor(external, true, false), "placement change resets history even when both inputs exclude UI");
    external.passes = 2;
    Require(history.ResetFor(external, true, false), "multipass change resets history");
    Require(!history.ResetFor(external, true, false), "multipass history persists across completed frames");
    external.reconstruction.inputScale = 0.5f;
    Require(history.ResetFor(external, true, false), "reconstruction scale change resets history");
    Require(history.ResetFor(external, false, false), "missing world breaks history");
    Require(history.ResetFor(external, true, false), "world re-entry resets history");
    external.beforeUpscaling = false; external.worldOnly = false;
    Require(history.ResetFor(external, true, false), "return to native composition resets history");

    using namespace TheosRenderPipeline::NeuralRendering;
    Reconstruction original;
    Require(EffectiveResolve(original) == ResolveMethod::Auto, "normal native-resolution Auto remains direct");
    auto producer = original;
    producer.producerColor = true;
    Require(EffectiveResolve(producer) == ResolveMethod::Ratio, "producer input always restores scene colour even with Auto");
    Require(!SameReconstructionResources(original, producer), "switching colour contract recreates the temporal feature");
    auto renormalized = producer;
    renormalized.whitePoint = 16;
    Require(!SameReconstructionResources(producer, renormalized), "normalization changes require retirement and recreation");
    auto strengthOnly = producer;
    strengthOnly.transferStrength = 0.5f;
    Require(SameReconstructionResources(producer, strengthOnly), "resolve strength does not change feature allocations");
    producer.method = ResolveMethod::Residual; producer.inputScale = 0.5f;
    Require(EffectiveResolve(producer) == ResolveMethod::Ratio, "reduced producer input cannot select clamping residual output");
    external.reconstruction = producer;
    Require(history.ResetFor(external, true, false), "producer colour contract resets temporal history");
    Require(!history.ResetFor(external, true, false), "stable producer contract retains history");

    RendererSettingsDraft draft;
    draft.valid = true; draft.sourceDLSSG.neuralEnabled = true;
    RendererSettingsCapabilities capabilities{true, true, false, true};
    Require(!ValidateRendererSettings(draft, capabilities), "external world NR does not require the native host's UI texture");
    draft.upscaleType = DLAA;
    Require(!ValidateRendererSettings(draft, capabilities), "saved native DLAA preference cannot lock NR when CS owns upscaling");
    capabilities.externalWorld = false;
    Require(ValidateRendererSettings(draft, capabilities), "native DLAA still requires dedicated UI");
    capabilities.dedicatedUI = true;
    Require(!ValidateRendererSettings(draft, capabilities), "native DLAA with dedicated UI must allow NR");
    capabilities.dedicatedUI = false;
    draft.upscaleType = DLSS;
    Require(ValidateRendererSettings(draft, capabilities), "native UI requirement preserved");
    capabilities.dedicatedUI = true;
    Require(!ValidateRendererSettings(draft, capabilities), "native DLSS and dedicated UI remain valid");
    capabilities.externalWorld = true; capabilities.neuralRuntime = false;
    Require(ValidateRendererSettings(draft, capabilities), "external world cannot bypass missing runtime check");
    capabilities.neuralRuntime = true; capabilities.sourceHost = false;
    Require(ValidateRendererSettings(draft, capabilities), "external world cannot bypass unavailable host check");

    for (int mode : {DLSS, DLAA}) for (bool before : {false, true}) {
        draft.upscaleType = mode;
        draft.sourceDLSSG.neuralBeforeUpscaling = before;
        Require(SupportsNeuralRenderingMode(mode, false), "native DLSS and DLAA share the startup/apply NR mode policy");
        for (unsigned mask = 0; mask < 16; ++mask) {
            const bool host = mask & 1, runtime = mask & 2, ui = mask & 4, cs = mask & 8;
            const bool accepted = ValidateRendererSettings(draft, {host, runtime, ui, cs}) == nullptr;
            Require(accepted == (host && runtime && (ui || cs)), "both placements preserve host/runtime/UI requirements in DLSS and DLAA");
        }
    }
    Require(!SupportsNeuralRenderingMode(-1, false) && !SupportsNeuralRenderingMode(1, false),
        "unrecognised native modes remain ineligible for NR");
    Require(SupportsNeuralRenderingMode(-1, true), "CS mode ownership remains independent of the native setting");
    draft.upscaleType = -1;
    Require(ValidateRendererSettings(draft, {true, true, true, false}), "invalid startup selection is rejected");

    NeuralRuntimeAvailability availability;
    NeuralOptions request;
    request.runtimePath = "legacy.dll";
    int probes = 0;
    auto build = RuntimeBuild::Legacy;
    const auto classify = [&](const std::filesystem::path&) { ++probes; return build; };
    Require(!availability.UnavailableReason(request, classify) && probes == 0, "NR off does not read the runtime");
    request.enabled = true; request.worldOnly = request.beforeUpscaling = true;
    request.reconstruction.producerColor = true;
    Require(availability.UnavailableReason(request, classify), "legacy early CS is rejected before feature creation");
    Require(probes == 1 && request.enabled, "preflight preserves the user's NR request");
    for (int frame = 0; frame < 1000; ++frame) {
        Require(availability.UnavailableReason(request, classify), "unsupported frames consistently skip inference");
    }
    Require(probes == 1, "steady frames do not read or hash a runtime again");
    request.beforeUpscaling = false; request.reconstruction.producerColor = false;
    Require(!availability.UnavailableReason(request, classify), "switching to legacy late Auto recovers without relaunch");
    request.reconstruction.inputScale = 0.5f;
    Require(availability.UnavailableReason(request, classify), "legacy reduced Auto requires unsupported residual reconstruction");
    request.reconstruction.inputScale = 1; request.reconstruction.method = ResolveMethod::Ratio;
    Require(availability.UnavailableReason(request, classify), "legacy explicit Ratio is rejected too");
    request.reconstruction.method = ResolveMethod::Auto; request.beforeUpscaling = true; request.worldOnly = false;
    Require(!availability.UnavailableReason(request, classify), "native legacy early Auto is preserved");
    Require(probes == 1, "placement and reconstruction controls reuse the cached identity");
    request.reconstruction.producerColor = true;
    for (auto modern : {RuntimeBuild::Build14, RuntimeBuild::Nexus3108}) {
        build = modern; availability.Reset();
        for (float scale : {1.0f, 0.5f}) for (int passes : {1, 2}) {
            request.reconstruction.inputScale = scale; request.passes = passes;
            Require(!availability.UnavailableReason(request, classify), "modern early NR preserves resolution and multipass support");
        }
    }
    Require(probes == 3, "explicit refresh probes each new identity once");
    request.runtimePath = "unknown.dll"; build = RuntimeBuild::Unknown;
    Require(availability.UnavailableReason(request, classify) && probes == 4, "new unrecognised runtime is rejected without loading it");
    request.enabled = false;
    Require(!availability.UnavailableReason(request, classify) && probes == 4, "turning NR off clears unavailable status without I/O");
    request.enabled = true; build = RuntimeBuild::Nexus3108; availability.Reset();
    Require(!availability.UnavailableReason(request, classify) && probes == 5, "re-enable can refresh a replaced runtime");
    std::puts("NR world contract: native/external placement, temporal changes, UI ownership and missing runtime/host checks passed.");
}
