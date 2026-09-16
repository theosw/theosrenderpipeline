#pragma once

#include "DLSSPreset.h"
#include <algorithm>

namespace TheosRenderPipeline::Upscaler
{
    // Preserve existing TheosRenderPipeline INI mode values: 0 = DLSS, 3 = DLAA.
    struct Creation
    {
        int mode{0}, quality{2}, preset{11};
        bool sharpening{true}, autoExposure{true};
        bool operator==(const Creation&) const = default;
        int AllocationQuality() const { return mode == 3 ? 5 : quality; }
    };

    inline Creation Sanitize(Creation value)
    {
        value.mode = value.mode == 3 ? 3 : 0;
        value.quality = std::clamp(value.quality, 0, 4);
        value.preset = TheosRenderPipeline::DLSSPreset::Sanitize(value.preset);
        return value;
    }

    class Configuration
    {
    public:
        void Initialize(Creation value)
        {
            requested_ = submitted_ = effective_ = persisted_ = startup_ = Sanitize(value);
            initialized_ = true;
            ready_ = failed_ = false;
        }
        void Request(Creation value) { requested_ = Sanitize(value); }
        void Saved() { persisted_ = requested_; }
        const Creation& Requested() const { return requested_; }
        const Creation& Submitted() const { return submitted_; }
        const Creation& Effective() const { return effective_; }
        const Creation& Persisted() const { return persisted_; }
        const Creation& Startup() const { return startup_; }
        bool Initialized() const { return initialized_; }
        bool Ready() const { return ready_ && !failed_; }
        bool Failed() const { return failed_; }
        bool Unsaved() const { return requested_ != persisted_; }
        bool NeedsRestart() const
        {
            return requested_.mode != startup_.mode ||
                (requested_.mode != 3 && requested_.quality != startup_.quality);
        }
        Creation LiveCandidate() const
        {
            auto value = requested_;
            value.mode = startup_.mode;
            value.quality = startup_.quality;
            return value;
        }
        bool NeedsLiveChange() const { return Ready() && LiveCandidate() != effective_; }
        Creation BeginSubmission() { return submitted_ = LiveCandidate(); }
        void Completed(bool success)
        {
            ready_ = success;
            failed_ = !success;
            if (success) { effective_ = submitted_; }
        }
    private:
        Creation requested_, submitted_, effective_, persisted_, startup_;
        bool initialized_{}, ready_{}, failed_{};
    };

    // One production/test boundary: a failed retirement must never reach feature
    // creation, and failed creation/resume must never claim effective settings.
    template<class Retire, class Create, class Resume>
    bool ApplyLive(Configuration& configuration, Retire retire, Create create, Resume resume)
    {
        if (!configuration.NeedsLiveChange()) { return !configuration.Failed(); }
        if (!retire()) { configuration.Completed(false); return false; }
        const auto request = configuration.BeginSubmission();
        if (!create(request) || !resume()) { configuration.Completed(false); return false; }
        configuration.Completed(true);
        return true;
    }
}
