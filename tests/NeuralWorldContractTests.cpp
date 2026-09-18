#include "RendererSettings.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
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
    Require(ValidateRendererSettings(draft, capabilities), "native DLAA NR restriction preserved");
    draft.upscaleType = DLSS;
    Require(ValidateRendererSettings(draft, capabilities), "native UI requirement preserved");
    capabilities.dedicatedUI = true;
    Require(!ValidateRendererSettings(draft, capabilities), "native DLSS and dedicated UI remain valid");
    capabilities.externalWorld = true; capabilities.neuralRuntime = false;
    Require(ValidateRendererSettings(draft, capabilities), "external world cannot bypass missing runtime check");
    capabilities.neuralRuntime = true; capabilities.sourceHost = false;
    Require(ValidateRendererSettings(draft, capabilities), "external world cannot bypass unavailable host check");
    std::puts("NR world contract: native/external placement, temporal changes, UI ownership and missing runtime/host checks passed.");
}
