#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace TheosRenderPipeline
{
    enum class NRModelActivity : std::uint8_t { Unknown, Ran, Skipped };
    inline const char* ModelActivityName(NRModelActivity value)
    {
        return value == NRModelActivity::Ran ? "ran" : value == NRModelActivity::Skipped ? "skipped" : "unknown";
    }

    // A host Present experiment, not a display pacer. Learn the mean cost of
    // complete model/skip pairs. Wait after this frame's GPU work is submitted,
    // before Present and CPU retirement, so the hold overlaps pending GPU work
    // instead of adding a sleep to an already retired frame. Keep CPU retirement:
    // the alternating-load SDK fixture's GPU-wait variant had worse tails and FPS.
    // Intel's private output 0 may still differ; game acceptance is separate.
    class XeFGModelBeat
    {
    public:
        static constexpr unsigned kWarmupPairs = 16;
        static constexpr double kMaximumIntervalUs = 100000;
        // Don't cap an already even (often CPU-limited) frame stream. A signed
        // pair difference rejects jitter unrelated to model phase. Once pacing
        // starts it stays armed until Reset, avoiding on/off feedback oscillation.
        static constexpr double kMinimumPhaseDifferenceUs = 500, kRelativePhaseDifference = .03;
        struct Decision { double deadlineUs{}, targetUs{}; bool paced{}; };
        void Reset() { *this = {}; }
        Decision Plan(double nowUs, NRModelActivity model, bool reset = false)
        {
            if (reset || !std::isfinite(nowUs) || nowUs <= 0 || model == NRModelActivity::Unknown) {
                Reset(); return {};
            }
            if (lastUs_ && (nowUs <= lastUs_ || nowUs - lastUs_ > kMaximumIntervalUs * 2 || model == lastModel_)) Reset();
            const double interval = lastUs_ ? nowUs - lastUs_ : 0;
            // During calibration lastUs_ is unpaced; afterwards it is the actual
            // release. Pair averages remove phase bias. Never extrapolate a hitch.
            if (interval > 0) {
                if (halfUs_ > 0) {
                    const double pair = (halfUs_ + interval) / 2;
                    if (pair >= 1000 && pair <= kMaximumIntervalUs) {
                        samples_[nextPair_] = pair;
                        phaseDifferences_[nextPair_] = model == NRModelActivity::Ran ? interval - halfUs_ : halfUs_ - interval;
                        nextPair_ = (nextPair_ + 1) % kWarmupPairs;
                        pairs_ = (std::min)(pairs_ + 1, kWarmupPairs);
                        if (pairs_ == kWarmupPairs) {
                            auto sorted = samples_;
                            std::sort(sorted.begin(), sorted.end());
                            const double typical = (sorted[7] + sorted[8]) / 2;
                            auto phases = phaseDifferences_;
                            std::sort(phases.begin(), phases.end());
                            phaseDifferenceUs_ = std::abs((phases[7] + phases[8]) / 2);
                            if (!targetUs_) {
                                if (phaseDifferenceUs_ >= (std::max)(kMinimumPhaseDifferenceUs, typical * kRelativePhaseDifference))
                                    targetUs_ = typical;
                            }
                            // A single streaming hitch must not become a lasting
                            // slower beat. Require the rolling pair median first.
                            else if (typical > targetUs_) targetUs_ += (typical - targetUs_) / 8;
                        }
                    }
                    halfUs_ = 0;
                } else halfUs_ = interval;
            }
            lastModel_ = model;
            // Each paced frame is at least one beat after the last release. A
            // miss rebases instead of issuing a burst of catch-up Presents.
            const double deadline = targetUs_ && lastUs_ ? (std::max)(nowUs, lastUs_ + targetUs_) : nowUs;
            lastUs_ = nowUs;
            return {deadline, targetUs_, targetUs_ > 0};
        }
        void Released(double nowUs) { if (std::isfinite(nowUs) && nowUs >= lastUs_) lastUs_ = nowUs; }
        double TargetUs() const { return targetUs_; }
        double PhaseDifferenceUs() const { return phaseDifferenceUs_; }
        unsigned PairSamples() const { return pairs_; }
    private:
        std::array<double, kWarmupPairs> samples_{};
        std::array<double, kWarmupPairs> phaseDifferences_{};
        unsigned pairs_{}, nextPair_{};
        double lastUs_{}, halfUs_{}, targetUs_{}, phaseDifferenceUs_{};
        NRModelActivity lastModel_{};
    };
}
