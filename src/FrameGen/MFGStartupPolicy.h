#pragma once
#include <string_view>

namespace TheosRenderPipeline::SourceDLSSG
{
    // Why the RTX 20/30 compatibility path cannot start. The first two are
    // decided from the setting and the driver version alone, before any NVIDIA
    // module is loaded. (A GPU CUDA cannot identify is not a refusal: it takes
    // NVIDIA's own route without TRP's unlocks.)
    enum class MFGStartupRefusal { CompatibilityDisabled, DriverTooOld, PreparationFailed };

    // True when only frame generation has to go: Streamline is never loaded and
    // the session continues on the startup transport. False keeps the fatal
    // dialog. Preparation publishes patched provider data and then resolver
    // imports; a refusal before the first publication changed nothing, while a
    // rollback or any later failure leaves NVIDIA code in a state only a restart clears.
    constexpr bool CanContinueWithoutFrameGeneration(MFGStartupRefusal refusal, bool providerPublished, std::string_view error)
    {
        if (refusal != MFGStartupRefusal::PreparationFailed) { return true; }
        const auto has = [&](std::string_view part) { return error.find(part) != std::string_view::npos; };
        return !providerPublished && !has("restart required") && !has("rolled back") && !has("rollback") &&
            !has("cannot be repeated");
    }
}
