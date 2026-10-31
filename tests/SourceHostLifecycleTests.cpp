#include "FrameGen/SourceHostLifecycle.h"
#include "FrameGen/SourceDLSSGSettings.h"
#include "FrameGen/SlowPresentGate.h"
#include "FrameGen/XeFGSettleGate.h"
#include <SimpleIni.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
struct Operations {
    std::vector<std::string> calls;
    HRESULT release{S_OK}, failure{S_OK};
    bool retired{true}, gameFacing{true}, presentation{true}, session{true}, upscaler{true};
    void DisableGeneration() { calls.emplace_back("disable"); }
    bool Retire() { calls.emplace_back("retire"); return retired; }
    void EndUI() { calls.emplace_back("end-ui"); }
    void ClearAndFlush() { calls.emplace_back("clear"); }
    HRESULT ReleaseUpscaler() {
        calls.emplace_back("release-upscaler");
        if (SUCCEEDED(release)) { upscaler = false; }
        return release;
    }
    void ReleaseGameFacing() { calls.emplace_back("release-game"); gameFacing = false; }
    void ReleasePresentation() { calls.emplace_back("release-presentation"); presentation = false; }
    HRESULT Fail(HRESULT result, const char*) { calls.emplace_back("fail"); if (SUCCEEDED(failure)) { failure = result; } return failure; }
    void UnpublishInput() { calls.emplace_back("unpublish"); }
    void DetachFailedHost() { calls.emplace_back("detach-failed"); }
    void BeginDestruction() { calls.emplace_back("destroy"); }
    void DetachSwapchains() { calls.emplace_back("detach-swapchains"); }
    void ResetSession() { calls.emplace_back("reset-session"); session = false; }
    bool RebuildGameFacing() { calls.emplace_back("rebuild-game"); return true; }
    bool RebuildUpscaler() { calls.emplace_back("rebuild-upscaler"); return true; }
    void RequestHistoryReset() { calls.emplace_back("reset-history"); }
};
int main()
{
    using namespace TheosRenderPipeline;
    {
        Operations o;
        Require(SourceHostLifecycle::BeforeResize(o) == S_OK, "successful resize retirement");
        Require(o.calls == std::vector<std::string>{"disable", "retire", "end-ui", "clear", "release-upscaler", "release-game", "release-presentation"}, "resize ordering");
        Require(SourceHostLifecycle::AfterResize(o, S_OK) == S_OK && o.calls.back() == "reset-history", "rebuild resets history");
    }
    for (auto failure : {E_OUTOFMEMORY, DXGI_ERROR_DEVICE_REMOVED}) {
        Operations o; o.release = failure;
        Require(SourceHostLifecycle::BeforeResize(o) == failure && o.failure == failure, "original destruction error reaches resize caller");
        Require(o.gameFacing && o.presentation && o.upscaler && o.session, "failed release retains host ownership");
        Require(o.calls.back() == "fail", "no release after failed upscaler destruction");
    }
    {
        Operations o; o.retired = false;
        Require(SourceHostLifecycle::BeforeResize(o) == DXGI_ERROR_WAS_STILL_DRAWING && o.calls.size() == 2, "failed retirement releases nothing");
    }
    {
        Operations o; o.release = E_OUTOFMEMORY;
        Require(!SourceHostLifecycle::Destroy(o) && o.failure == E_OUTOFMEMORY, "destruction preserves failed SDK result");
        Require(o.calls.back() == "detach-failed" && o.gameFacing && o.presentation && o.session && o.upscaler, "failed destruction avoids session reset and resource release");
        o.release = S_OK;
        Require(SourceHostLifecycle::Destroy(o) && !o.gameFacing && !o.presentation && !o.session && !o.upscaler, "later successful retirement can release retained owners");
        Require(o.failure == E_OUTOFMEMORY, "cleanup retry does not erase original failure");
    }
    {
        Operations o; o.retired = false;
        Require(!SourceHostLifecycle::Destroy(o) && o.calls == std::vector<std::string>{"unpublish", "retire", "detach-failed"}, "failed destruction retirement preserves all resources");
    }
    for (auto provider : {FrameGenerationProvider::NVIDIA, FrameGenerationProvider::XeFG}) {
        CSimpleIniA ini;
        SourceDLSSG::Preferences settings;
        settings.provider = provider;
        SourceDLSSG::StorePreferences(ini, settings);
        const auto saved = SourceDLSSG::LoadPreferences(ini);
        const auto xeSS = SelectProviderStartup(true, true, saved.provider);
        Require(xeSS.requested == provider && xeSS.presenter == provider, "saved XeSS provider is honored at startup without a return transition");
        const auto dlss = SelectProviderStartup(false, true, saved.provider);
        Require(dlss.requested == provider && dlss.presenter == FrameGenerationProvider::NVIDIA, "existing DLSS startup bridge preserves requested provider");
        const auto amd = SelectProviderStartup(true, false, saved.provider);
        Require(amd.requested == FrameGenerationProvider::XeFG && amd.presenter == FrameGenerationProvider::XeFG, "non-NVIDIA XeSS startup never requires NVIDIA provider");
        // Community Shaders upscales on its route, so TRP's XeSS is off there.
        const auto amdCS = SelectProviderStartup(false, false, saved.provider);
        Require(amdCS.requested == FrameGenerationProvider::XeFG && amdCS.presenter == FrameGenerationProvider::XeFG, "non-NVIDIA startup without TRP XeSS starts XeFG directly");
    }
    Require(SelectProviderStartup(false, false, static_cast<FrameGenerationProvider>(42)).presenter == FrameGenerationProvider::XeFG, "invalid provider on non-NVIDIA still starts XeFG");
    Require(SelectProviderStartup(true, true, static_cast<FrameGenerationProvider>(42)).requested == FrameGenerationProvider::NVIDIA, "invalid provider keeps configured default policy");    // 2026-10-03: XeSS start, NR on, then Intel -> NVIDIA failed MFG verification fatally.
    for (const bool enabled : {false, true}) {
        CSimpleIniA ini;
        ini.SetLongValue("FrameGeneration", "Provider", 0);
        ini.SetBoolValue("FrameGeneration", "Enabled", enabled);
        const auto saved = SourceDLSSG::LoadPreferences(ini);
        auto session = SelectProviderStartup(true, false, saved.provider);
        // Applying another setting must not requeue the unavailable saved default.
        session.requested = NormalizeProviderRequest(saved.provider, false);
        Require(session.requested == session.presenter && session.requested == FrameGenerationProvider::XeFG,
            "Pascal/non-NVIDIA startup and later Apply have no pending unavailable provider, even with FG off");
        Require(saved.provider == FrameGenerationProvider::NVIDIA && ini.GetLongValue("FrameGeneration", "Provider", -1) == 0,
            "session/menu normalization does not rewrite saved NVIDIA defaults");
    }
    Require(!XeFGStartupInvertedDepth(false), "native Skyrim's first forward-Z camera needs no startup recreation");
    Require(XeFGStartupInvertedDepth(true), "external CS projection retains its provisional startup convention");
    Require(NormalizeProviderRequest(FrameGenerationProvider::NVIDIA, true) == FrameGenerationProvider::NVIDIA &&
        NormalizeProviderRequest(FrameGenerationProvider::XeFG, true) == FrameGenerationProvider::XeFG,
        "available NVIDIA keeps both live provider choices");
    Require(NvidiaSwitchNeedsRestart(false, true), "NR-initialized NGX before Streamline refuses the live NVIDIA switch");
    Require(!NvidiaSwitchNeedsRestart(false, false), "XeSS start without NR still switches to NVIDIA live");
    Require(!NvidiaSwitchNeedsRestart(true, true), "Streamline initialized before NR keeps live switching");
    {
        SlowPresentGate gate;
        Require(!gate.Admit(99.9, 0), "a Present under 100 ms is not reported");
        Require(gate.Admit(100.0, 1000), "the first slow Present is reported");
        Require(!gate.Admit(250.0, 1200) && !gate.Admit(300.0, 1499), "slow Presents within 500 ms are counted, not logged");
        Require(!gate.Admit(50.0, 1450), "fast Presents are neither logged nor counted");
        Require(gate.Admit(120.0, 1500) && gate.TakeSuppressed() == 2, "the next line reports the slow Presents it skipped");
        Require(gate.TakeSuppressed() == 0, "the skipped count resets once reported");
    }
    {
        // RC3 CS route: ~550 ms frames after a new presenter, then normal frames.
        XeFGSettleGate gate;
        for (int i = 0; i < 6; ++i) Require(!gate.Observe(550.0), "slow frames keep XeFG in passthrough");
        for (unsigned i = 1; i < XeFGSettleGate::kRequired; ++i) Require(!gate.Observe(30.0), "fewer than ten settled frames still wait");
        Require(gate.Observe(30.0) && gate.Held() == 6 + XeFGSettleGate::kRequired, "ten settled frames start generation");
        Require(gate.Observe(800.0) && gate.Settled(), "a later hitch does not toggle generation off mid-play");
        gate.Reset();
        Require(!gate.Settled() && gate.Held() == 0, "a new presenter or transition waits again");
        for (unsigned i = 0; i + 1 < XeFGSettleGate::kRequired; ++i) gate.Observe(30.0);
        Require(!gate.Observe(0.0) && !gate.Observe(150.0), "an unknown or slow interval restarts the count");
    }
    std::puts("PASS host failure retention, resize/destruction ordering, saved provider startup, slow Present reporting and XeFG settling");
}
