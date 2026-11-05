#pragma once
#include "sdk_pacing_dlssg.h"

namespace sdk_pacing::cadence {
// A sync-only observation boundary. Keep the first output's arrival separate
// from the SDK stamp taken after its Present/fence waits. The estimator itself
// stays shared with the ordinary cadence implementation; this adapter owns no
// wait, resource or presentation operation.
class VSyncCadence {
public:
    void Reset() noexcept { *this = {}; }
    bool Begin(uint32_t frame, uint32_t outputs, uint32_t sync, int64_t ready) noexcept {
        if (outputs < 3 || outputs > 4 || sync < 1 || sync > 4 || ready <= 0) { Reset(); return false; }
        if (sync_ && sync != sync_) interval_.Reset();
        frame_ = frame; outputs_ = outputs; sync_ = sync; ready_ = ready;
        interval_.Observe(frame, outputs, ready);
        return true;
    }
    bool Matches(uint32_t frame, uint32_t outputs) const noexcept {
        return ready_ > 0 && frame_ == frame && outputs_ == outputs;
    }
    bool Matches(uint32_t frame, uint32_t outputs, uint32_t sync) const noexcept {
        return Matches(frame, outputs) && sync_ == sync;
    }
    bool Plan(uint32_t frame, uint32_t outputs, uint32_t index, int64_t now,
        int64_t batchStart, int64_t gpuLatency, BatchIntervalEstimator::Decision& decision) const noexcept {
        if (!Matches(frame, outputs) || batchStart < ready_ || now < batchStart
            || now - ready_ > BatchIntervalEstimator::kMaximumDelta) return false;
        // Arrival measures cadence; the completed first output places this
        // batch on the presentation timeline. Using arrival for both releases
        // inner outputs before the first image is ready and recreates bursts.
        // Translate the clock, not the estimator, so its one-interval cap and
        // the SDK's Present-to-fence latency credit keep their existing meaning.
        const auto offset = batchStart - ready_;
        if (!interval_.Plan(frame, outputs, index, now - offset, decision, gpuLatency)) return false;
        decision.deadline += offset;
        return true;
    }
    const BatchIntervalEstimator& Interval() const noexcept { return interval_; }
    int64_t Ready() const noexcept { return ready_; }
private:
    BatchIntervalEstimator interval_;
    uint32_t frame_{}, outputs_{}, sync_{};
    int64_t ready_{};
};
}
