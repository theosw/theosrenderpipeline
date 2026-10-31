#include "StartupHints.h"
#include "FrameGen/NvidiaDriverVersion.h"
#include <cstdio>
#include <cstdlib>

using namespace TheosRenderPipeline::StartupHints;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
static bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

int main()
{
    // Status lines as reported by users and produced by the renderer.
    Require(Classify("another frame-generation mod is already loaded; disable the competing mod failed HRESULT=0x8000FFFF") ==
        Failure::CompetingFrameGeneration, "competing frame generation");
    Require(Classify("sl.interposer.dll already loaded by another owner failed HRESULT=0x8000FFFF") ==
        Failure::StreamlineOwnedElsewhere, "Streamline owned elsewhere");
    Require(Classify("slSetFeatureLoaded feature=1000 Streamline result=34 failed HRESULT=0x80004005") ==
        Failure::FrameGenerationUnavailable, "result 34");
    Require(!Classify("slSetFeatureLoaded feature=3 Streamline result=34 failed HRESULT=0x80004005"), "Reflex load is not the DLSS-G hint");
    Require(Classify("nvngx_dlssg.dll was not loaded from the configured Streamline directory failed HRESULT=0x80004005") ==
        Failure::StreamlineModuleMissing, "module missing");
    Require(Classify("NVIDIA swapchain ownership was not acquired failed (0x80004005); rendering stopped, restart required.") ==
        Failure::SwapchainNotAcquired, "swapchain not acquired");
    Require(Classify("configured DLSS-G module or required export missing; no patches applied") ==
        Failure::CompatibilityRuntimeMissing, "Ada runtime missing");
    Require(Classify("Ampere resolver publication changed; restart required") == Failure::CompatibilityRuntimeChanged, "import rewrite");
    Require(Classify("Ampere publication verification failed") == Failure::CompatibilityRuntimeChanged, "publication verification");
    Require(Classify("NVAPI does not match the physical compatibility adapter") == Failure::CompatibilityAdapterUnidentified, "NVAPI");
    Require(Classify("Ampere provider ABI is unsupported") == Failure::CompatibilityRuntimeUnsupported, "unsupported runtime");
    Require(Classify("NVIDIA driver 566.36 is too old for RTX 20/30 frame generation") == Failure::DriverTooOld, "old driver");
    Require(Has(Hint(Failure::DriverTooOld, {}), "572.16 or newer"), "old driver hint names the version to install");

    // DXGI reports NVIDIA's 566.36 as 32.0.15.6636 and 610.88 as 32.0.16.1088.
    {
        using namespace TheosRenderPipeline::SourceDLSSG;
        const auto low = [](unsigned third, unsigned fourth) { return static_cast<std::uint32_t>(third << 16 | fourth); };
        Require(NvidiaDriverVersion(low(15, 6636)) == 56636 && NvidiaDriverVersion(low(16, 1088)) == 61088 &&
            NvidiaDriverVersion(low(15, 7216)) == 57216, "NVIDIA driver numbers decode from the user-mode version");
        Require(NvidiaDriverVersion(0) == 0 && NvidiaDriverVersion(low(16, 12000)) == 0, "malformed versions are unknown");
        Require(DriverVersionText(56636) == "566.36" && DriverVersionText(57005) == "570.05", "versions print as NVIDIA writes them");
        Require(!BackportDriverSupported(56636) && !BackportDriverSupported(57064) && BackportDriverSupported(57065) && BackportDriverSupported(57216) &&
            BackportDriverSupported(61088), "RTX 20/30 frame generation needs the Windows CUDA 12.8 driver or newer");
        Require(BackportDriverSupported(0), "an unknown version is not refused");
    }
    Require(!Classify("Deferred source DLSS startup failed") && HintFor("Deferred source DLSS startup failed", {}).empty(),
        "unrecognised failures keep the original dialog");

    Facts standard{};
    const auto unsupported = Hint(Failure::FrameGenerationUnavailable, Facts{false, false, true, false, true, {}});
    Require(Has(unsupported, "Likely causes and what to try:\n") && Has(unsupported, "reported DLSS frame generation as unsupported") &&
        Has(unsupported, "Universal edition") && Has(unsupported, "Smooth Motion") && Has(unsupported, "scheduling is off"),
        "result 34 on Standard: runtime reason, Universal, overrides and HAGS off");
    const auto universal = Hint(Failure::FrameGenerationUnavailable, Facts{true, false, false, true, false, {}});
    Require(!Has(universal, "Universal edition") && !Has(universal, "as unsupported") && Has(universal, "NVIDIA reported a frame-generation override") &&
        Has(universal, "Check that hardware-accelerated GPU scheduling is on"), "only observed facts are stated as facts");
    Require(Has(Hint(Failure::StreamlineOwnedElsewhere, Facts{.otherOwner = "D:\\Mods\\ENB AA\\sl.interposer.dll"}), "ENB AA\\sl.interposer.dll") &&
        Has(Hint(Failure::StreamlineOwnedElsewhere, standard), "ENB Anti-Aliasing"), "names the other owner when known");
    Require(Has(Hint(Failure::SwapchainNotAcquired, Facts{.communityShaders = true}), "Community Shaders' frame generation") &&
        Has(Hint(Failure::SwapchainNotAcquired, standard), "ENB frame generation"), "CS route vs other swapchain owners");
    const auto cs = Hint(Failure::SwapchainNotAcquired, Facts{.communityShaders = true});
    Require(Has(cs, "Presets > Baseline") && Has(cs, "\"Theo's Render Pipeline\"") && Has(cs, "CS 1.9.1") &&
        Has(cs, "under Upscaling") && Has(cs, "temporarily disable TRP") && Has(cs, "re-enable TRP and restart"),
        "both CS generations and the inaccessible-menu recovery path");
    Require(!Has(Hint(Failure::SwapchainNotAcquired, standard), "Presets > Baseline"), "CS help requires observed CS");
    const auto translated = Hint(Failure::SwapchainNotAcquired, Facts{.communityShaders = true},
        [](const char*) -> const char* { return "translated"; });
    Require(!Has(translated, "Skyrim") && Has(translated, "translated- translated"), "CS prose and heading reach the dictionary");
    Require(Has(Hint(Failure::CompatibilityRuntimeChanged, standard), "Apocrypha Menu Framework"), "import rewriting mods");
    for (auto failure : {Failure::CompetingFrameGeneration, Failure::StreamlineOwnedElsewhere, Failure::FrameGenerationUnavailable,
             Failure::StreamlineModuleMissing, Failure::SwapchainNotAcquired, Failure::CompatibilityRuntimeMissing,
             Failure::CompatibilityRuntimeChanged, Failure::CompatibilityRuntimeUnsupported, Failure::CompatibilityAdapterUnidentified}) {
        const auto hint = Hint(failure, Facts{false, false, true, true, true, "C:\\x\\sl.common.dll"});
        Require(!hint.empty() && hint.size() < 700, "every failure has a compact hint");
    }
    std::puts("PASS startup failure classification and hints");
}
