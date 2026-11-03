#include "FrameGen/NeuralFrameEligibility.h"
#include "RendererSettings.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
#include "FrameGen/SourceDLSSGNeuralAvailability.h"
#include "FrameGen/NativeUIFrame.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <limits>

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
    external.passes = 5;
    Require(history.ResetFor(external, true, false) && !history.ResetFor(external, true, false), "five-pass history");
    external.reconstruction.inputScale = 0.5f;
    Require(history.ResetFor(external, true, false), "reconstruction scale change resets history");
    Require(history.ResetFor(external, false, false), "missing world breaks history");
    Require(history.ResetFor(external, true, false), "world re-entry resets history");
    external.beforeUpscaling = false; external.worldOnly = false;
    Require(history.ResetFor(external, true, false), "return to native composition resets history");

    // Replay world frames, hidden interface/console, loading, and return.
    // The same eligibility helpers are used by both native host and backend.
    for (bool early : {false, true}) {
        NeuralOptions options; options.enabled = true; options.beforeUpscaling = early;
        NeuralHistory timeline;
        unsigned evaluations = 0;
        const auto frame = [&](bool menu, bool guides) {
            const bool eligible = NeuralFrameEligible(true, options.WorldOnly(), true, menu, guides, guides);
            const bool reset = timeline.ResetFor(options, eligible, false);
            const bool evaluatedEarly = eligible && options.WorldOnly();
            const bool prepared = eligible && NeuralPreparedEligible(options.WorldOnly(), evaluatedEarly, guides, guides);
            if (prepared) ++evaluations;
            return reset;
        };
        Require(frame(false, true), "initial world resets history");
        Require(frame(false, false) == !early, "missing UI guides preserve early NR history and suspend late NR");
        Require(!frame(false, false), "repeated hidden-interface frames are stable");
        Require(frame(false, true) == !early, "late NR restarts when its guides return");
        Require(frame(true, false), "loading interrupts NR history in either placement");
        Require(!frame(true, false), "loading remains suspended");
        Require(frame(false, true), "return from loading resets NR");
        Require(evaluations == (early ? 5u : 3u), "only eligible complete frames evaluate");
        Require(!NeuralFrameEligible(true, early, false, false, true, true), "unavailable UI state is not an active world");
        Require(!NeuralPreparedEligible(true, false, true, true), "world-only cannot claim an unevaluated early pass");
        Require(!NeuralFrameEligible(false, early, true, false, true, true), "dedicated native UI contract remains required");
    }

    // A hidden interface (tm, the console, photo modes) hands off a cleared UI
    // layer and the HUD-less scene, so late NR keeps running; a failed clear
    // or a UI that is neither drawn nor hidden hands off nothing.
    Require(HandOffUIAtPresent(true, false, false), "drawn UI is handed off");
    Require(HandOffUIAtPresent(false, true, true), "a hidden interface hands off a transparent UI layer");
    Require(!HandOffUIAtPresent(false, true, false), "an uncleared UI layer is never handed off");
    Require(!HandOffUIAtPresent(false, false, false), "an undrawn, visible interface keeps the old fallback");
    {
        NeuralOptions late; late.enabled = true;
        const bool guides = HandOffUIAtPresent(false, true, true);
        Require(NeuralFrameEligible(true, late.WorldOnly(), true, false, guides, guides), "late NR stays eligible with the interface hidden");
    }

    using namespace TheosRenderPipeline::NeuralRendering;
    Reconstruction original;
    Require(EffectiveResolve(original) == ResolveMethod::Auto, "normal native-resolution Auto remains direct");
    auto peripheral = original;
    peripheral.peripheralCompression = true;
    Require(!original.peripheralCompression, "peripheral mode is opt-in");
    Require(ModelExtent(5120, peripheral) == 4608 && ModelExtent(1440, peripheral) == 1296, "80/90 work budget");
    Require(ModelExtent(1, peripheral) == 1 && ModelExtent(0, peripheral) == 0, "tiny and invalid extents");
    Require(EffectiveResolve(peripheral) == ResolveMethod::Residual, "peripheral Auto reconstructs the full scene");
    Require(!SameReconstructionResources(original, peripheral), "changing spatial layout recreates feature resources");
    peripheral.inputScale = 0.25f;
    Require(ModelExtent(5120, peripheral) == 1152, "global scale combines with peripheral work budget without re-clamping");
    external.reconstruction = peripheral;
    Require(history.ResetFor(external, true, false), "peripheral enable resets model history");
    Require(!history.ResetFor(external, true, false), "stable peripheral layout preserves history");
    for (float scale : {0.0f, 0.25f, 0.58f, 0.9f, 1.0f, 2.0f}) {
        auto uniform = original; uniform.inputScale = scale;
        Require(ModelExtent(2035, uniform) == WorkExtent(2035, scale), "off preserves existing rounded uniform dimensions");
    }
    Require(NormalizeInputScale(1.25f) == 1.25f && NormalizeInputScale(2.0f) == 2.0f && NormalizeInputScale(3.0f) == 2.0f,
        "input above 100% is kept up to 200%");
    Require(NormalizeInputScale(0.0f) == 1.0f && NormalizeInputScale(-1.0f) == 1.0f && NormalizeInputScale(0.1f) == 0.25f &&
        NormalizeInputScale(std::numeric_limits<float>::infinity()) == 1.0f && NormalizeInputScale(std::nanf("")) == 1.0f,
        "invalid input scales still mean native");
    {
        auto enlarged = original; enlarged.inputScale = 1.25f;
        Require(ModelExtent(5120, enlarged) == 6400 && ModelExtent(1440, enlarged) == 1800, "125% enlarges the model raster");
        Require(EffectiveResolve(enlarged) == ResolveMethod::Residual, "an enlarged model raster resolves back to native");
        Require(!SameReconstructionResources(original, enlarged), "enlarging recreates the feature");
        Require(SanitizeReconstruction(enlarged).inputScale == 1.25f, "sanitizing keeps an enlarged scale");
        auto packed = enlarged; packed.peripheralCompression = true;
        Require(ModelExtent(5120, packed) == 4608, "peripheral packing stops at native density");
    }
    auto producer = original;
    producer.producerColor = true;
    Require(EffectiveResolve(producer) == ResolveMethod::Ratio, "producer input always restores scene colour even with Auto");
    Require(!SameReconstructionResources(original, producer), "switching colour contract recreates the temporal feature");
    auto saved = original; saved.method = ResolveMethod::Residual; saved.whitePoint = 4; saved.inputScale = 0.5f;
    Require(CompletedSceneContract(saved, false) == saved, "SDR completed scene keeps the saved reconstruction");
    const auto hdrScene = CompletedSceneContract(saved, true);
    Require(hdrScene.producerColor && hdrScene.whitePoint == 1 && EffectiveResolve(hdrScene) == ResolveMethod::Ratio,
        "extended-range completed scene restores through the producer proxy at paper white");
    Require(hdrScene.inputScale == saved.inputScale && hdrScene.preset == saved.preset, "HDR contract keeps saved scale and preset");
    Require(!SameReconstructionResources(saved, hdrScene), "entering HDR recreates the temporal feature");
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

    for (int mode : {DLSS, DLAA, XeSS}) for (bool before : {false, true}) {
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
    request.reconstruction.peripheralCompression = true;
    Require(availability.UnavailableReason(request, classify), "legacy peripheral reconstruction rejected before GPU recording");
    request.reconstruction.peripheralCompression = false;
    Require(!availability.UnavailableReason(request, classify), "legacy direct mode remains available after disabling the experiment");
    request.reconstruction.inputScale = 0.5f;
    Require(availability.UnavailableReason(request, classify), "legacy reduced Auto requires unsupported residual reconstruction");
    request.reconstruction.inputScale = 1; request.passes = 2;
    request.extraPasses[0].linked = false; request.extraPasses[0].inputScale = .5f;
    Require(availability.UnavailableReason(request, classify), "legacy custom pass resolution rejected before GPU work");
    request.extraPasses[0].linked = true;
    Require(!availability.UnavailableReason(request, classify), "legacy linked passes recover without restart");
    request.passes = 4; request.extraPasses[2].linked = false; request.extraPasses[2].inputScale = .5f;
    Require(availability.UnavailableReason(request, classify), "legacy custom Pass 4 resolution rejected before GPU work");
    request.passes = 3;
    Require(!availability.UnavailableReason(request, classify), "an unused pass's resolution does not block legacy NR");
    request.passes = 2; request.extraPasses[2].linked = true;
    request.reconstruction.inputScale = 1; request.reconstruction.method = ResolveMethod::Ratio;
    Require(availability.UnavailableReason(request, classify), "legacy explicit Ratio is rejected too");
    request.reconstruction.method = ResolveMethod::Auto; request.beforeUpscaling = true; request.worldOnly = false;
    Require(!availability.UnavailableReason(request, classify), "native legacy early Auto is preserved");
    Require(probes == 1, "placement and reconstruction controls reuse the cached identity");
    request.reconstruction.producerColor = true;
    for (auto modern : {RuntimeBuild::Build14, RuntimeBuild::Nexus3108}) {
        build = modern; availability.Reset();
        for (float scale : {1.0f, 0.5f}) for (int passes : {1, 2, 5}) {
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

    // ConfigureNeuralRendering resets availability on each off/on. An initialized
    // pass supplies the build of its retained module only for the bound request.
    for (int toggle = 0; toggle < 10; ++toggle) {
        request.enabled = false;
        Require(!availability.UnavailableReason(request, classify, RuntimeBuild::Nexus3108), "retained runtime is inactive when disabled");
        request.enabled = true; availability.Reset();
        Require(!availability.UnavailableReason(request, classify, RuntimeBuild::Nexus3108), "retained modern runtime resumes");
    }
    Require(probes == 5, "enable-only toggles do not hash the retained runtime again");
    availability.Reset();
    Require(availability.UnavailableReason(request, classify, RuntimeBuild::Legacy), "retained identity still enforces reconstruction admission");
    Require(probes == 5, "retained legacy admission requires no disk probe");
    request.runtimePath = "changed.dll"; build = RuntimeBuild::Unknown;
    Require(availability.UnavailableReason(request, classify) && probes == 6, "changed request without a matching retained feature is verified and rejected");
    request.enabled = false;
    Require(!availability.UnavailableReason(request, classify) && probes == 6, "failed runtime stays dormant while off");
    request.enabled = true; build = RuntimeBuild::Nexus3108; availability.Reset();
    Require(!availability.UnavailableReason(request, classify) && probes == 7, "unsupported replacement can still recover on enable");
    availability.Reset();
    Require(!availability.UnavailableReason(request, classify) && probes == 8, "retired feature cannot supply a cached module identity");
    std::puts("NR world contract: native/external placement, temporal changes, UI ownership and missing runtime/host checks passed.");
}
