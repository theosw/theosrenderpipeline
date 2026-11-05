#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace TheosRenderPipeline::NeuralRendering
{
    // Recording time alone cannot distinguish a slow frame from a missing one.
    // Application IDs catch omitted recordings; the clock catches stalls where
    // no application frame advanced. Camera/menu resets still come from the host.
    class NeuralFrameContinuity
    {
    public:
        using Clock = std::chrono::steady_clock;
        bool Record(Clock::time_point now, std::optional<std::uint32_t> frame = {})
        {
            bool gap = previousFrame_ && frame && *frame != std::uint32_t(*previousFrame_ + 1);
            if (previousTime_) {
                const auto elapsed = now - *previousTime_;
                // Bootstrap without mistaking the first low-FPS interval for a
                // stall. Once sampled, four times the median of recent paired
                // intervals tolerates alternating model/skip costs and jitter.
                auto threshold = Clock::duration(std::chrono::seconds(1));
                if (count_) {
                    auto sorted = intervals_;
                    std::sort(sorted.begin(), sorted.begin() + count_);
                    threshold = (std::max)(Clock::duration(std::chrono::milliseconds(100)), sorted[count_ / 2] * 4);
                }
                gap |= elapsed < Clock::duration::zero() || elapsed > threshold;
                if (gap) {
                    // A new scene may stay slower. Relearn its cadence instead
                    // of repeatedly comparing it with the old fast scene.
                    count_ = next_ = 0;
                    previousInterval_.reset();
                } else if (elapsed > Clock::duration::zero()) {
                    if (previousInterval_) {
                        intervals_[next_] = (elapsed + *previousInterval_) / 2;
                        next_ = (next_ + 1) % intervals_.size();
                        count_ = (std::min)(count_ + 1, intervals_.size());
                    }
                    previousInterval_ = elapsed;
                }
            }
            previousTime_ = now;
            previousFrame_ = frame;
            return gap;
        }
    private:
        std::optional<Clock::time_point> previousTime_;
        std::optional<Clock::duration> previousInterval_;
        std::optional<std::uint32_t> previousFrame_;
        std::array<Clock::duration, 8> intervals_{};
        std::size_t count_{}, next_{};
    };
}
