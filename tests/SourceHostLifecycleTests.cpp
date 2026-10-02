#include "FrameGen/SourceHostLifecycle.h"
#include "FrameGen/SourceDLSSGSettings.h"
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
    }
    Require(SelectProviderStartup(true, true, static_cast<FrameGenerationProvider>(42)).requested == FrameGenerationProvider::NVIDIA, "invalid provider keeps configured default policy");
    std::puts("PASS host failure retention, resize/destruction ordering and saved provider startup");
}
