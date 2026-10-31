#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace TheosRenderPipeline::StartupHints
{
    // Fatal startup failures users most often report, recognised from the
    // technical status text that the dialog already shows.
    enum class Failure
    {
        CompetingFrameGeneration, // another frame-generation plugin is loaded
        StreamlineOwnedElsewhere, // a Streamline module was loaded by another owner first
        FrameGenerationUnavailable, // slSetFeatureLoaded for DLSS-G (feature 1000) failed
        StreamlineModuleMissing, // a configured Streamline module did not load from TRP's folder
        SwapchainNotAcquired, // device creation bypassed TRP's swapchain hook
        CompatibilityRuntimeMissing, // Universal could not find Standard's DLSS-G runtime
        CompatibilityRuntimeChanged, // another module changed patched code or imports
        CompatibilityRuntimeUnsupported, // NVIDIA runtime differs from the supported build
        CompatibilityAdapterUnidentified, // NVAPI could not match the rendering GPU
        DriverTooOld, // RTX 20/30 frame generation needs a newer NVIDIA driver
    };

    // What the renderer actually observed when the failure happened.
    struct Facts
    {
        bool universal{}; // this renderer includes the RTX 20/30/40 compatibility paths
        bool communityShaders{}; // CommunityShaders.dll is loaded
        bool frameGenerationUnsupportedReported{}; // Streamline logged DLSS-G as unsupported here
        bool frameGenerationOverrideReported{}; // Streamline logged an NVIDIA frame-generation override
        bool hardwareSchedulingOff{}; // Windows reports HAGS explicitly disabled
        std::string otherOwner; // path of an already loaded same-name module from another folder
    };

    inline bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

    inline std::optional<Failure> Classify(std::string_view status)
    {
        if (Contains(status, "another frame-generation mod is already loaded")) { return Failure::CompetingFrameGeneration; }
        if (Contains(status, "already loaded by another owner")) { return Failure::StreamlineOwnedElsewhere; }
        if (Contains(status, "slSetFeatureLoaded feature=1000")) { return Failure::FrameGenerationUnavailable; }
        if (Contains(status, "was not loaded from the configured Streamline directory")) { return Failure::StreamlineModuleMissing; }
        if (Contains(status, "swapchain ownership was not acquired")) { return Failure::SwapchainNotAcquired; }
        if (Contains(status, "configured DLSS-G module or required export missing") ||
            Contains(status, "prepared Ampere modules changed")) { return Failure::CompatibilityRuntimeMissing; }
        if (Contains(status, "is too old for RTX 20/30 frame generation")) { return Failure::DriverTooOld; }
        if (Contains(status, "NVAPI")) { return Failure::CompatibilityAdapterUnidentified; }
        if (Contains(status, "publication changed") || Contains(status, "publication verification failed") ||
            Contains(status, "resolver changed") || Contains(status, "export changed identity") ||
            Contains(status, "Streamline bypassed")) { return Failure::CompatibilityRuntimeChanged; }
        if (Contains(status, "provider ABI is unsupported") || Contains(status, "contracts changed") ||
            Contains(status, "patch targets are missing") || Contains(status, "minimum-architecture instruction is unsupported")) {
            return Failure::CompatibilityRuntimeUnsupported;
        }
        return std::nullopt;
    }

    // A few plain lines for the fatal dialog. Claims rest on Facts; anything
    // the renderer cannot observe is phrased as a likely cause.
    using Translate = const char* (*)(const char*);

    inline std::string Hint(Failure failure, const Facts& facts, Translate translate = nullptr)
    {
        std::string hint;
        auto line = [&](const char* text) { hint += "- "; hint += translate ? translate(text) : text; hint += '\n'; };
        switch (failure) {
        case Failure::CompetingFrameGeneration:
            line("PureDark's Upscaler or frame generation is loaded. Disable it in this profile; TRP replaces it.");
            break;
        case Failure::StreamlineOwnedElsewhere:
            hint += facts.otherOwner.empty() ? std::string("- Another mod loaded NVIDIA Streamline first.\n") :
                "- Another mod loaded NVIDIA Streamline first, from:\n  " + facts.otherOwner + '\n';
            line("Usually ENB Anti-Aliasing (DLAA/FSR), another upscaler or frame-generation mod, or a DLSS injector. "
                 "Disable it; TRP replaces ENB anti-aliasing.");
            break;
        case Failure::FrameGenerationUnavailable:
            if (facts.frameGenerationUnsupportedReported) { line("NVIDIA's runtime reported DLSS frame generation as unsupported on this system."); }
            if (!facts.universal) { line("RTX 20/30 cards need the Universal edition installed below Standard in MO2."); }
            if (facts.frameGenerationOverrideReported) {
                line("NVIDIA reported a frame-generation override. In NVIDIA App's Skyrim profile, set the DLSS overrides to "
                     "Use the 3D application setting.");
            } else {
                line("Likely an NVIDIA App or driver override: set Skyrim's DLSS overrides to Use the 3D application setting, "
                     "turn Smooth Motion off and disable global DLSS override tools.");
            }
            line(facts.hardwareSchedulingOff ?
                "Hardware-accelerated GPU scheduling is off. Turn it on in Windows graphics settings and restart Windows." :
                "Check that hardware-accelerated GPU scheduling is on in Windows graphics settings.");
            break;
        case Failure::StreamlineModuleMissing:
            line("Install Standard's matching NVIDIA runtime files. In MO2, install Universal below Standard if you use Universal.");
            line("Check which mod supplies the runtime files and remove incompatible replacements from other mods or manual fixes.");
            break;
        case Failure::SwapchainNotAcquired:
            if (facts.communityShaders) {
                line("Community Shaders' frame generation may still be on or forced. TRP cannot safely continue this session.");
                line("Newer CS / Bottled Shaders: enable \"Theo's Render Pipeline\" under Presets > Baseline, or turn Frame Generation off under Upscaling. Restart Skyrim.");
                line("CS 1.9.1: turn Frame Generation off under CS Upscaling and restart Skyrim.");
                line("If Skyrim closes before CS opens: temporarily disable TRP, start Skyrim and turn Frame Generation off under CS Upscaling. Close Skyrim, re-enable TRP and restart.");
            } else {
                line("Another mod likely created the game's swapchain first, such as ENB frame generation, OptiScaler or another "
                     "frame-generation mod. Disable it.");
            }
            break;
        case Failure::CompatibilityRuntimeMissing:
            line("NVIDIA's frame-generation runtime from Standard was not found. Install Standard with Universal below it "
                 "in MO2's left pane, so Universal wins file conflicts.");
            break;
        case Failure::CompatibilityRuntimeChanged:
            line("Another mod likely changed NVIDIA's runtime code or imports after loading, as Apocrypha Menu Framework "
                 "before 1.9.7 did. Update or disable such mods, overlays and DLL injectors, then restart.");
            break;
        case Failure::CompatibilityRuntimeUnsupported:
            line("The NVIDIA runtime files differ from the version this build supports. Reinstall Standard's files and remove "
                 "any replacement nvngx_dlssg.dll or Streamline files from other mods.");
            break;
        case Failure::CompatibilityAdapterUnidentified:
            line("NVIDIA's driver did not identify the GPU Skyrim renders on. Update the NVIDIA driver; on laptops, make "
                 "Skyrim use the NVIDIA GPU (Windows graphics settings: High performance).");
            break;
        case Failure::DriverTooOld:
            line("Update the NVIDIA driver to 572.16 or newer (the latest is best) through NVIDIA App or nvidia.com, "
                 "then start Skyrim again.");
            break;
        }
        return hint.empty() ? hint : std::string(translate ? translate("Likely causes and what to try:\n") : "Likely causes and what to try:\n") + hint;
    }

    // Gathers Facts from the running process (renderer only; not used by tests).
    Facts CurrentFacts(std::string_view status);
    // Reads only TRP's language preference; technical logs remain English.
    std::string DialogHintFor(std::string_view status, const Facts& facts);

    // Empty when the status is not a recognised startup failure.
    inline std::string HintFor(std::string_view status, const Facts& facts)
    {
        const auto failure = Classify(status);
        return failure ? Hint(*failure, facts) : std::string{};
    }
}
