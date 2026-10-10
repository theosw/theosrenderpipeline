#pragma once
#include <cstdint>
#include <utility>

namespace TheosRenderPipeline
{
    // Rate-limited report of host Presents long enough to be felt as a hitch.
    // Rolling percentiles hide which frame stalled; this names it, at most one
    // line per interval, counting the slow Presents it skipped in between.
    class SlowPresentGate
    {
    public:
        static constexpr double kThresholdMs = 100.0;
        static constexpr std::int64_t kIntervalMs = 500;
        bool Admit(double totalMs, std::int64_t nowMs)
        {
            if (totalMs < kThresholdMs) { return false; }
            if (logged_ && nowMs - lastMs_ < kIntervalMs) { ++suppressed_; return false; }
            logged_ = true; lastMs_ = nowMs;
            return true;
        }
        std::uint32_t TakeSuppressed() { return std::exchange(suppressed_, 0u); }
    private:
        std::int64_t lastMs_{};
        std::uint32_t suppressed_{};
        bool logged_{};
    };
}
