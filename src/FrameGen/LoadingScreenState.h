#pragma once

#include <cstdint>

namespace TheosRenderPipeline
{
    class LoadingWorldReadiness
    {
    public:
        bool Boundary(std::uint64_t frame, bool world, bool mainMenu, bool loadingMenu) noexcept
        {
            if (mainMenu) {
                sawMain_ = true;
                ResetAfterRetirement();
                return false;
            }
            if (world && frame != lastWorld_) {
                lastWorld_ = frame;
                if (loadingMenu) { worldFrames_ = 0; }
                else if (sawMain_ && worldFrames_ < 120) {
                    if (++worldFrames_ == 120) { ready_ = true; }
                }
            }
            return ready_;
        }

        void ResetAfterRetirement() noexcept
        {
            worldFrames_ = 0;
            lastWorld_ = ~std::uint64_t{};
            ready_ = false;
        }

    private:
        std::uint64_t lastWorld_{~std::uint64_t{}};
        unsigned worldFrames_{};
        bool sawMain_{}, ready_{};
    };

    // The route is valid only for the background boundary that selected it.
    // Keep the history reset pending until a temporal evaluation succeeds.
    class LoadingScreenRoute
    {
        LoadingWorldReadiness readiness_;
        std::uint64_t frame_{~std::uint64_t{}};
        bool temporalReset_{};
    public:
        void Boundary(std::uint64_t frame, bool world, bool loading, bool main)
        {
            const bool settled = readiness_.Boundary(frame, world, main, loading);
            frame_ = !world && loading && !main && settled ? frame : ~std::uint64_t{};
        }
        bool Active(std::uint64_t frame) const { return frame_ == frame; }
        bool NeedsTemporalReset() const { return temporalReset_; }
        void SpatialSucceeded() { temporalReset_ = true; }
        void TemporalSucceeded() { temporalReset_ = false; }
        void ResetAfterRetirement()
        {
            frame_ = ~std::uint64_t{};
            temporalReset_ = true;
            readiness_.ResetAfterRetirement();
        }
    };
}
