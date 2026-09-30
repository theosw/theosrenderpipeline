#pragma once

#include <algorithm>

namespace TheosRenderPipeline
{
    // Fade from black when TRP has asked the game to show loading artwork for a
    // door/interior transition. The game shows those loading screens abruptly,
    // without the fade-in of its own loading screens, and their first frames can
    // flash bright before the artwork appears. Times are seconds on any clock.
    class LoadingFadeIn
    {
    public:
        static constexpr double kDuration = 0.5;
        // A forced transition that never opens the loading menu expires.
        static constexpr double kPendingLimit = 5.0;

        void RequestFade(double now) noexcept { pending_ = true; requested_ = now; }

        // Returns the brightness factor for this frame: 0 = black, 1 = unchanged.
        float Update(bool loadingMenuOpen, double now) noexcept
        {
            if (pending_ && now - requested_ > kPendingLimit) { pending_ = false; }
            if (pending_ && loadingMenuOpen) { pending_ = false; active_ = true; start_ = now; }
            if (!active_) { return 1.0f; }
            const double t = std::clamp((now - start_) / kDuration, 0.0, 1.0);
            if (t >= 1.0) { active_ = false; return 1.0f; }
            // Ease in: the first frames, where the flash appears, stay near black.
            return static_cast<float>(t * t);
        }

        bool Active() const noexcept { return active_; }
        void Reset() noexcept { *this = {}; }

    private:
        double requested_{}, start_{};
        bool pending_{}, active_{};
    };
}
