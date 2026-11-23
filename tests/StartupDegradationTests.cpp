#include "FrameGen/StartupFallback.h"
#include "FrameGen/MFGStartupPolicy.h"
#include "FrameGen/LoadingArtworkPolicy.h"
#include "SessionDegradationText.h"
#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::SourceDLSSG;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
static bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// Startup failures that used to close Skyrim and now cost one feature for the
// session, and the ones that must stay fatal.
int main()
{
    // XeFG's startup check runs before any presenter factory call.
    {
        const HRESULT missingLibrary = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        for (const HRESULT refused : {missingLibrary, static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED), static_cast<HRESULT>(E_NOINTERFACE), static_cast<HRESULT>(E_FAIL)}) {
            Require(CanDegradeXeFGProbe(refused, S_OK, false, false),
                "missing libxess_fg.dll, no SM6.4, x2 unavailable or an SDK refusal continue on the startup transport");
        }
        Require(!CanDegradeXeFGProbe(E_UNEXPECTED, S_OK, false, false), "an Unsafe XeFG result stays fatal");
        Require(!CanDegradeXeFGProbe(DXGI_ERROR_UNSUPPORTED, S_OK, true, false), "a probe context that could not be destroyed stays fatal");
        Require(!CanDegradeXeFGProbe(DXGI_ERROR_UNSUPPORTED, S_OK, false, true), "never after a presenter factory call");
        Require(!CanDegradeXeFGProbe(DXGI_ERROR_UNSUPPORTED, DXGI_ERROR_DEVICE_REMOVED, false, false), "never on a removed device");
        Require(!CanDegradeXeFGProbe(DXGI_ERROR_DEVICE_REMOVED, S_OK, false, false) && !CanDegradeXeFGProbe(E_POINTER, S_OK, false, false) &&
            !CanDegradeXeFGProbe(S_OK, S_OK, false, false), "device loss, contract errors and success are not degradations");
    }

    // RTX 20/30: refusals known before anything is loaded or patched.
    {
        using enum MFGStartupRefusal;
        Require(CanContinueWithoutFrameGeneration(DriverTooOld, false, "NVIDIA driver 566.36 is too old for RTX 20/30 frame generation") &&
            CanContinueWithoutFrameGeneration(CompatibilityDisabled, false, "SourceDLSSGMFGUnlock=false disables its compatibility path"),
            "driver too old and unlock off cost frame generation only");
        // Preparation: as trp::ampere::Start reports its failures.
        for (const char* early : {"NVAPI could not be loaded", "NVAPI does not match the physical compatibility adapter",
                "Ampere provider ABI is unsupported", "Ampere provider has no retargetable programs",
                "Ampere resolver import is missing or ambiguous", "Ampere startup preparation failed"}) {
            Require(CanContinueWithoutFrameGeneration(PreparationFailed, false, early), "a preparation refused before any publication is not fatal");
        }
        for (const char* late : {"Ampere provider rollback failed; restart required", "Ampere provider preparation rolled back",
                "Ampere resolver installation failed; restart required", "Ampere resolver rollback failed; restart required",
                "Ampere startup preparation raised an exception; restart required", "Ampere startup cannot be repeated"}) {
            Require(!CanContinueWithoutFrameGeneration(PreparationFailed, false, late), "a publication, rollback or repeated start stays fatal");
        }
        Require(!CanContinueWithoutFrameGeneration(PreparationFailed, true, "Ampere startup preparation failed"),
            "once provider data was published, a failure stays fatal whatever its text");

        // The refusals enter the startup transport: their status carries the failure code the transport accepts.
        Require(CanUseStartupTransport(DXGI_ERROR_UNSUPPORTED, S_OK, false), "the refusal's result may use the startup transport");
        // And the menu gets the matching advice from the same status text.
        const auto text = [](const char* status) {
            return Degradation::Explain({Degradation::Reason::FrameGenerationStartup, std::string(status) + " failed HRESULT=0x887A0004"}, {});
        };
        Require(Has(text("NVIDIA driver 566.36 is too old for RTX 20/30 frame generation"), "572.16 or newer"), "driver advice");
        Require(Has(text("RTX 20/30-series detected, but SourceDLSSGMFGUnlock=false disables its compatibility path. Set it to true in "
            "the winning MO2 INI (including Overwrite) and restart"), "SourceDLSSGMFGUnlock=true"), "unlock advice");
        Require(Has(text("active rendering adapter could not be identified; no patches applied (code 3)"), "did not identify the GPU"), "adapter advice");
        Require(Has(text("NVAPI could not be loaded"), "did not identify the GPU"), "NVAPI preparation advice");
        Require(Has(text("Ampere provider ABI is unsupported"), "differ from the version this build supports"), "unsupported runtime advice");
    }

    // Loading artwork: optional, refused before anything is written.
    {
        using namespace LoadingArtwork;
        Require(InstallRefusal(true, true) == nullptr, "a known runtime with the expected code installs the hook");
        Require(std::string_view(InstallRefusal(true, false)) == "Loading artwork hook does not match the engine transition sender",
            "changed engine code refuses the hook");
        Require(std::string_view(InstallRefusal(false, false)) == "No verified loading artwork profile exists for this Skyrim runtime" &&
            InstallRefusal(false, true) != nullptr, "an unknown runtime refuses the hook");
        Require(kTransitionPrologue.size() == 19 && kTransitionPrologue[0] == 0x40 && kTransitionPrologue[18] == 0xFF, "the checked prologue is unchanged");
        // Without the hook the suppression flag is never altered, because the thunk never runs;
        // the policy itself is unchanged for the installed case.
        Require(Suppression(kAECallers, true, true, true, kAECallers.exterior, 1, 1, 1) == 0 &&
            Suppression(kAECallers, false, true, true, kAECallers.exterior, 1, 1, 1) == 1, "installed behaviour is unchanged");
    }
    std::puts("StartupDegradation tests passed");
    return 0;
}
