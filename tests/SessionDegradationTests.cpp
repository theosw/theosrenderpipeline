#include "SessionDegradation.h"
#include "SessionDegradationText.h"
#include <cstdio>
#include <cstdlib>

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::Degradation;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
static bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

int main()
{
    // The record: one entry per reason, in first-reported order, never removed.
    {
        Record record;
        Require(record.Entries().empty() && record.Revision() == 0, "a new record is empty");
        Require(record.Report(Reason::GuideRefused, "depth copy refused"), "a new reason is news");
        Require(!record.Report(Reason::GuideRefused, "depth copy refused"), "the same report again is not logged twice");
        Require(record.Revision() == 1, "a repeated report changes nothing");
        Require(record.Report(Reason::NeuralRenderingStopped, "NR refused"), "a second reason is added");
        Require(record.Report(Reason::GuideRefused, "motion copy refused"), "a new detail for a known reason is news");
        const auto entries = record.Entries();
        Require(entries.size() == 2 && entries[0].reason == Reason::GuideRefused && entries[0].detail == "motion copy refused" &&
            entries[1].reason == Reason::NeuralRenderingStopped, "one entry per reason, in the order first reported");

        Require(record.Active(Reason::GuideRefused) && !record.Active(Reason::FrameGenerationStartup), "active only once reported");
        Require(record.Resolve(Reason::GuideRefused) && !record.Resolve(Reason::GuideRefused), "resolving is reported once");
        Require(!record.Resolve(Reason::LoadingArtworkUnavailable), "resolving an unreported reason does nothing");
        Require(!record.Active(Reason::GuideRefused) && record.Entries().size() == 2 && !record.Entries()[0].active,
            "a resolved entry stays for Copy details, marked inactive");
        const auto revision = record.Revision();
        Require(record.Report(Reason::GuideRefused, "motion copy refused") && record.Active(Reason::GuideRefused) &&
            record.Revision() == revision + 1, "a resolved reason that returns is news again");
    }

    // Which pipeline box turns red.
    Require(FeatureOf(Reason::FrameGenerationStartup) == Feature::FrameGeneration &&
        FeatureOf(Reason::MultiFrameGeneration) == Feature::FrameGeneration &&
        FeatureOf(Reason::GuideRefused) == Feature::FrameGeneration &&
        FeatureOf(Reason::NeuralRenderingStopped) == Feature::NeuralRendering &&
        FeatureOf(Reason::PackagedSettingsMissing) == Feature::Settings &&
        FeatureOf(Reason::LegacySettingsIgnored) == Feature::Settings &&
        FeatureOf(Reason::LoadingArtworkUnavailable) == Feature::LoadingArtwork, "each reason names its feature");
    Require(Affects(Reason::FrameGenerationStartup, Feature::NeuralRendering) && Affects(Reason::GuideRefused, Feature::NeuralRendering) &&
        !Affects(Reason::MultiFrameGeneration, Feature::NeuralRendering) && !Affects(Reason::NeuralRenderingStopped, Feature::FrameGeneration),
        "startup transport and a refused guide also stop NR; an MFG stand-down does not");

    // The log line keeps the technical status verbatim.
    {
        const auto line = LogLine(Reason::FrameGenerationStartup, "slInit Streamline result=5 failed HRESULT=0x80004005");
        Require(Has(line, "[Degraded] frame generation:") && Has(line, "slInit Streamline result=5 failed HRESULT=0x80004005") &&
            Has(line, "saved settings are unchanged"), "log line names the feature, the status and that settings are kept");
    }

    // Frame-generation startup reuses the startup dialog's advice.
    {
        StartupHints::Facts facts; facts.universal = true; facts.hardwareSchedulingOff = true;
        const auto text = Explain({Reason::FrameGenerationStartup, "slSetFeatureLoaded feature=1000 Streamline result=34 failed HRESULT=0x80004005"}, facts);
        Require(Has(text, "Frame generation could not start") && Has(text, "The game runs normally") &&
            Has(text, "saved settings are unchanged"), "says what is off and that the game continues");
        Require(Has(text, "Likely causes and what to try:") && Has(text, "Hardware-accelerated GPU scheduling is off") &&
            Has(text, "Use the 3D application setting"), "feature 1000: the dialog's advice reaches the menu, with observed facts");
        Require(!Has(text, "Universal edition"), "advice that does not apply is left out");
        Require(text.back() != '\n', "no trailing blank line");

        const auto driver = Explain({Reason::FrameGenerationStartup, "NVIDIA driver 566.36 is too old for RTX 20/30 frame generation failed HRESULT=0x887A0004"}, {});
        Require(Has(driver, "572.16 or newer"), "RTX 20/30 driver too old: says which driver to install");
        const auto unlock = Explain({Reason::FrameGenerationStartup, "RTX 20/30-series detected, but SourceDLSSGMFGUnlock=false disables its "
            "compatibility path. failed HRESULT=0x887A0004"}, {});
        Require(Has(unlock, "SourceDLSSGMFGUnlock=true") && Has(unlock, "Overwrite"), "unlock off: says which setting to change and where");
        const auto adapter = Explain({Reason::FrameGenerationStartup, "active rendering adapter could not be identified; no patches applied failed HRESULT=0x887A0004"}, {});
        Require(Has(adapter, "did not identify the GPU"), "adapter unidentified: driver and laptop GPU advice");
        const auto xefg = Explain({Reason::FrameGenerationStartup, "Intel XeFG startup: SM6.4 unavailable failed HRESULT=0x887A0004"}, {});
        Require(Has(xefg, "Intel XeFG could not start") && Has(xefg, "libxess_fg.dll"), "XeFG: names the files to check");
        const auto unknown = Explain({Reason::FrameGenerationStartup, "upgrade factory failed HRESULT=0x80004005"}, {});
        Require(Has(unknown, "Restart Skyrim to try again") && Has(unknown, "Copy details"), "an unrecognised failure still says what to do");
        StartupHints::Facts smooth; smooth.smoothMotion = true;
        Require(Has(Explain({Reason::FrameGenerationStartup, "upgrade factory failed HRESULT=0x80004005"}, smooth), "Smooth Motion is on for Skyrim"),
            "Smooth Motion is named when it is loaded");
    }

    // Every other reason: what is off, that the game continues, what to do.
    {
        const auto mfg = Explain({Reason::MultiFrameGeneration, "MFG unavailable this session: wrapper patch failed. x2 frame generation continues."}, {});
        Require(Has(mfg, "above x2 are off") && Has(mfg, "continues at x2") && Has(mfg, "Reinstall TRP's own files"), "MFG stand-down");
        const auto overridden = Explain({Reason::MultiFrameGeneration, "MFG unavailable this session: x. x2 frame generation continues. "
            "NVIDIA app is overriding DLSS Frame Generation for Skyrim; set its Frame Generation overrides to Use the 3D application setting to restore MFG."}, {});
        Require(Has(overridden, "NVIDIA App replaces the frame-generation file") && !Has(overridden, "Reinstall TRP's own files"),
            "MFG stand-down under an NVIDIA App override points at the override");
        const auto gpu = Explain({Reason::AdapterUnidentified, "the rendering GPU could not be identified (code 3); no patches applied"}, {});
        Require(Has(gpu, "could not identify your NVIDIA GPU") && Has(gpu, "RTX 40 cards stay at x2") && Has(gpu, "RTX 50 cards are not affected") &&
            Has(gpu, "keeps running") && Has(gpu, "did not identify the GPU"), "GPU not identified: what each card keeps, plus driver/laptop advice");
        Require(FeatureOf(Reason::AdapterUnidentified) == Feature::FrameGeneration && !Affects(Reason::AdapterUnidentified, Feature::NeuralRendering),
            "GPU not identified marks frame generation only");
        const auto guide = Explain({Reason::GuideRefused, "Frame generation and NR are off: the depth copy was refused (0x80070057). Restart to retry."}, {});
        Require(Has(guide, "are paused") && Has(guide, "keeps running normally") && Has(guide, "Restart Skyrim"), "guide refusal");
        const auto retry = Explain({Reason::NeuralRenderingStopped, "NR feature creation failed; NR is off. Apply to retry; saved preference unchanged."}, {});
        Require(Has(retry, "Neural Rendering stopped") && Has(retry, "Press Apply to try again"), "NR stopped, retryable");
        const auto restart = Explain({Reason::NeuralRenderingStopped, "NR release refused; NR requires restart."}, {});
        Require(Has(restart, "Restart Skyrim to use Neural Rendering again") && !Has(restart, "Press Apply"), "NR stopped, restart needed");
        const auto ini = Explain({Reason::PackagedSettingsMissing, "rc=-3"}, {});
        Require(Has(ini, "built-in defaults") && Has(ini, "saved settings still apply") && Has(ini, "Reinstall"), "packaged INI missing");
        const auto legacy = Explain({Reason::LegacySettingsIgnored, "Settings/EnableUpscaler, Debug/AutoABTest"}, {});
        Require(Has(legacy, "Settings/EnableUpscaler, Debug/AutoABTest.") && Has(legacy, "delete those lines"), "old settings: names the keys");
        const auto artwork = Explain({Reason::LoadingArtworkUnavailable, "hook mismatch"}, {});
        Require(Has(artwork, "Request loading artwork") && Has(artwork, "Everything else works"), "loading artwork");
    }

    // Translation reaches every sentence; the technical detail stays as written.
    {
        const auto marked = Explain({Reason::LegacySettingsIgnored, "Settings/EnableUpscaler"}, {}, [](const char*) { return "#"; });
        Require(marked == "# Settings/EnableUpscaler.\n#", "each sentence goes through the translator");
    }
    Require(std::string_view(BoxLabel(Reason::FrameGenerationStartup)) == "Off this session" &&
        std::string_view(BoxLabel(Reason::GuideRefused)) == "Paused", "pipeline box labels");
    std::puts("SessionDegradation tests passed");
    return 0;
}
