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
        value.mode = value.mode == 3 || value.mode == 4 || value.mode == 5 ? value.mode : 0;
        value.quality = std::clamp(value.quality, 0, 4);
        value.preset = TheosRenderPipeline::DLSSPreset::Sanitize(value.preset);
        return value;
    }

    // DLSS and DLAA need an NVIDIA GPU. Elsewhere XeSS runs for the session,
    // keeping the quality index (same order); DLAA takes Ultra Quality.
    inline Creation ForAdapter(Creation value, bool nvidiaAdapter)
    {
        value = Sanitize(value);
        if (nvidiaAdapter || value.mode == 4 || value.mode == 5) { return value; }
        if (value.mode == 3) { value.quality = 4; }
        value.mode = 4;
        return value;
    }

    // Apply before allocating targets or selecting the presentation provider.
    // XeSS is the existing vendor-neutral startup route and needs no DLSS preload.
    inline Creation ForFSRRuntime(Creation value, bool runtimePresent)
    {
        value = Sanitize(value);
        if (value.mode == 5 && !runtimePresent) { value.mode = 4; }
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
            rejectedMode_ = -1;
            nvidiaServicesReady_ = false;
        }
        void Request(Creation value)
        {
            value = Sanitize(value);
            if (value.mode != requested_.mode) { rejectedMode_ = -1; }
            requested_ = value;
        }
        void RejectLiveMode() { rejectedMode_ = requested_.mode; }
        // Host publishes this only after its Streamline/device/module services
        // initialize. It does not bypass the NR-first NVIDIA-FG startup guard.
        void SetNvidiaServicesReady(bool ready) { nvidiaServicesReady_ = ready; }
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
        bool LiveModeRejected() const { return requested_.mode == rejectedMode_; }
        bool NeedsRestart() const
        {
            return requested_.mode != LiveCandidate().mode ||
                (requested_.mode != 3 && requested_.quality != startup_.quality);
        }
        Creation LiveCandidate() const
        {
            auto value = requested_;
            const bool fixedAlgorithm = startup_.mode == 0 || startup_.mode == 4 || startup_.mode == 5;
            // Vendor-neutral methods can switch without initializing NVIDIA.
            // Entry to DLSS from either still requires services + SDK preflight.
            if (!fixedAlgorithm || value.mode == 3 ||
                (value.mode == 0 && startup_.mode != 0 && !nvidiaServicesReady_)) { value.mode = effective_.mode; }
            if (requested_.mode == rejectedMode_) { value.mode = effective_.mode; }
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
        int rejectedMode_{-1};
        bool nvidiaServicesReady_{};
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
