#include "FrameGen/FSRUpscaler.h"
#include "NvidiaUpscalerConfiguration.h"
#include "RendererSettings.h"
#include <cstdio>
#include <cstdlib>

using namespace TheosRenderPipeline;
static void Require(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
int main()
{
    Require(FSR == 5 && XeSS == 4 && DLAA == 3 && DLSS == 0, "stable INI values");
    for (int mode : {DLSS, DLAA, XeSS, FSR}) for (bool present : {false, true}) {
        const Upscaler::Creation requested{mode, 1, 11, true, false};
        auto expected = requested;
        if (mode == FSR && !present) { expected.mode = XeSS; }
        Require(Upscaler::ForFSRRuntime(requested, present) == expected,
            "missing optional FSR runtime falls back before allocation without changing tuning or other modes");
    }
    for (bool nvidia : {false, true}) for (int quality = 0; quality < 5; ++quality) {
        const Upscaler::Creation fsr{FSR, quality, 11, true, true};
        Require(Upscaler::Sanitize(fsr) == fsr && Upscaler::ForAdapter(fsr, nvidia) == fsr,
            "explicit FSR survives INI sanitizing and adapter selection");
    }
    for (int from : {DLSS, XeSS, FSR}) for (int to : {XeSS, FSR}) {
        Upscaler::Configuration c; c.Initialize({from, 2, 11, true, true}); c.BeginSubmission(); c.Completed(true);
        c.Request({to, 0, 11, false, false});
        Require(c.LiveCandidate().mode == to && c.LiveCandidate().quality == 2 && c.NeedsRestart(),
            "algorithm applies at fixed allocation; pending quality remains restart-bound");
        int stages = 0;
        Require(Upscaler::ApplyLive(c, [&] { ++stages; return true; }, [&](const auto& r) {
            Require(stages == 1 && r.mode == to, "retire before creation"); ++stages; return true;
        }, [&] { ++stages; return true; }) && stages == 3 && c.Effective().mode == to,
            "vendor-neutral transition needs no NVIDIA services");
        c.Saved(); Require(c.Persisted().mode == to && c.Persisted().quality == 0 && c.Effective().quality == 2,
            "Save preserves requested versus effective settings");
    }
    for (int from : {XeSS, FSR}) {
        Upscaler::Configuration c; c.Initialize({from, 2, 11, true, true}); c.BeginSubmission(); c.Completed(true);
        c.Request({DLSS, 2, 11, true, true});
        Require(c.NeedsRestart() && !c.NeedsLiveChange(), "cold NVIDIA entry still guarded");
        c.SetNvidiaServicesReady(true); Require(c.NeedsLiveChange(), "ready NVIDIA services admit SDK preflight");
        c.RejectLiveMode(); Require(!c.NeedsLiveChange() && c.Effective().mode == from, "preflight refusal retains active owner");
    }
    Upscaler::Configuration failed; failed.Initialize({XeSS,2,11,true,true}); failed.BeginSubmission(); failed.Completed(true);
    failed.Request({FSR,2,11,true,true}); bool created = false;
    Require(!Upscaler::ApplyLive(failed, [] { return false; }, [&](const auto&) { created=true;return true; }, [] { return true; }) &&
        !created && failed.Effective().mode == XeSS, "retirement failure never releases or replaces working upscaler");
    for (bool nvidia : {false,true}) for (bool before : {false,true}) {
        RendererSettingsDraft draft; draft.valid=true; draft.upscaleType=FSR;
        RendererSettingsCapabilities caps{true,true,true,false}; caps.nvidiaAdapter=nvidia; caps.nvidiaFeatures=false;
        Require(!ValidateRendererSettings(draft,caps), "FSR admitted independently of RTX features");
        draft.sourceDLSSG.neuralEnabled=true; draft.sourceDLSSG.neuralBeforeUpscaling=before;
        Require((ValidateRendererSettings(draft,caps)==nullptr)==nvidia, "FSR preserves research NR admission in both placements");
        caps.dedicatedUI=false; Require(ValidateRendererSettings(draft,caps), "FSR cannot bypass NR native UI requirement");
    }
    FrameExtent input{};
    UpscalerInputResolution range{};
    Require(SUCCEEDED(FSRUpscaler::QueryInputRange(1920,1080,2,range)) && range.Valid() &&
        range.optimal.width == 1280 && range.optimal.height == 720 &&
        range.Accepts({1272,716},{1920,1080}), "FSR admits NR-friendly fixed extents without querying DLSS");
    Require(FAILED(FSRUpscaler::QueryInputRange(0,1080,2,range)) && !range.Valid(),
        "invalid FSR query clears a previously valid range");
    Require(SUCCEEDED(FSRUpscaler::QuerySize(1280,720,1,input)) && input.width==752 && input.height==423,
        "official Balanced 1.7x ratio uses truncation");
    Require(FSRUpscaler::Accepts({753,424},{1280,720}) && FSRUpscaler::Accepts({1129,635},{1920,1080}),
        "XeSS Balanced fixed extents accepted for matched comparison");
    Require(!FSRUpscaler::Accepts({1280,800},{1920,1080}) && !FSRUpscaler::Accepts({0,424},{1280,720}) &&
        !FSRUpscaler::Accepts({1921,1080},{1920,1080}), "invalid aspects and extents rejected");
    Require(FAILED(FSRUpscaler::QuerySize(0,720,1,input)) && FAILED(FSRUpscaler::QuerySize(1280,720,5,input)),
        "invalid size and quality rejected");
    std::puts("PASS FSR settings, fixed-size transitions, NR/UI admission and allocation bounds");
}
