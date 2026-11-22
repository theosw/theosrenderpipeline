#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// What TRP is running without this session, and why. Written at the places
// that already degrade softly instead of stopping the game; read by the menu
// (red pipeline box and a plain explanation) and by Copy details. Nothing here
// changes a saved setting: the record describes this session only.
namespace TheosRenderPipeline::Degradation
{
    enum class Feature { Upscaling, FrameGeneration, NeuralRendering, Settings, LoadingArtwork };

    enum class Reason
    {
        FrameGenerationStartup,    // frame generation could not start; normal presentation without FG, Reflex and NR
        MultiFrameGeneration,      // x3 and above stood down; x2 continues
        AdapterUnidentified,       // GPU not identified; TRP's unlocks off, NVIDIA's own FG decides
        GuideRefused,              // a guide copy was refused; FG and NR pause until new guides
        NeuralRenderingStopped,    // NR turned itself off after a refusal
        PackagedSettingsMissing,   // TheosRenderPipeline.ini missing or unreadable; built-in defaults
        LegacySettingsIgnored,     // old keys in the settings files are not used
        LoadingArtworkUnavailable, // the loading-artwork hook was not installed
    };

    constexpr Feature FeatureOf(Reason reason)
    {
        switch (reason) {
        case Reason::FrameGenerationStartup:
        case Reason::MultiFrameGeneration:
        case Reason::AdapterUnidentified:
        case Reason::GuideRefused: return Feature::FrameGeneration;
        case Reason::NeuralRenderingStopped: return Feature::NeuralRendering;
        case Reason::PackagedSettingsMissing:
        case Reason::LegacySettingsIgnored: return Feature::Settings;
        case Reason::LoadingArtworkUnavailable: return Feature::LoadingArtwork;
        }
        return Feature::Settings;
    }
    // Startup transport and a refused guide also leave NR without its inputs.
    constexpr bool Affects(Reason reason, Feature feature)
    {
        return FeatureOf(reason) == feature || (feature == Feature::NeuralRendering &&
            (reason == Reason::FrameGenerationStartup || reason == Reason::GuideRefused));
    }
    constexpr const char* Name(Feature feature)
    {
        switch (feature) {
        case Feature::Upscaling: return "upscaling";
        case Feature::FrameGeneration: return "frame generation";
        case Feature::NeuralRendering: return "Neural Rendering";
        case Feature::Settings: return "settings";
        case Feature::LoadingArtwork: return "loading artwork";
        }
        return "unknown";
    }
    constexpr const char* Name(Reason reason)
    {
        switch (reason) {
        case Reason::FrameGenerationStartup: return "frame generation did not start";
        case Reason::MultiFrameGeneration: return "multi frame generation stood down to x2";
        case Reason::AdapterUnidentified: return "GPU not identified; TRP's frame-generation unlocks off";
        case Reason::GuideRefused: return "a frame-generation input copy was refused";
        case Reason::NeuralRenderingStopped: return "Neural Rendering stopped";
        case Reason::PackagedSettingsMissing: return "packaged settings file missing; built-in defaults";
        case Reason::LegacySettingsIgnored: return "old settings ignored";
        case Reason::LoadingArtworkUnavailable: return "loading artwork unavailable";
        }
        return "unknown";
    }

    struct Entry
    {
        Reason reason{};
        std::string detail; // the technical status, verbatim, as the log has it
        bool active{true};  // false once the feature came back (new guides, an NR retry)
        friend bool operator==(const Entry&, const Entry&) = default;
    };

    // One entry per reason, in the order first reported; never removed, so Copy
    // details can still name a degradation that later resolved.
    class Record
    {
    public:
        // True when this is news worth one log line: a new reason, a resolved one
        // returning, or the same reason with a different detail.
        bool Report(Reason reason, std::string_view detail)
        {
            std::scoped_lock lock(mutex_);
            for (auto& entry : entries_) {
                if (entry.reason != reason) { continue; }
                if (entry.active && entry.detail == detail) { return false; }
                entry.detail = detail; entry.active = true;
                ++revision_;
                return true;
            }
            entries_.push_back({reason, std::string(detail), true});
            ++revision_;
            return true;
        }
        // The feature works again. True when the reason was active.
        bool Resolve(Reason reason)
        {
            std::scoped_lock lock(mutex_);
            for (auto& entry : entries_) {
                if (entry.reason == reason && entry.active) { entry.active = false; ++revision_; return true; }
            }
            return false;
        }
        std::vector<Entry> Entries() const { std::scoped_lock lock(mutex_); return entries_; }
        bool Active(Reason reason) const
        {
            std::scoped_lock lock(mutex_);
            for (const auto& entry : entries_) { if (entry.reason == reason) { return entry.active; } }
            return false;
        }
        // Changes with every Report or Resolve that altered the record.
        std::uint32_t Revision() const { std::scoped_lock lock(mutex_); return revision_; }

    private:
        mutable std::mutex mutex_;
        std::vector<Entry> entries_;
        std::uint32_t revision_{};
    };

    // The running game's record. Tests build their own Record.
    inline Record& Session()
    {
        static Record record;
        return record;
    }

    // One line for TheosRenderPipeline.log, written by the reporting site.
    inline std::string LogLine(Reason reason, std::string_view detail)
    {
        std::string line = "[Degraded] ";
        line += Name(FeatureOf(reason)); line += ": "; line += Name(reason);
        if (!detail.empty()) { line += " ("; line += detail; line += ')'; }
        line += "; the game continues and saved settings are unchanged";
        return line;
    }
}
