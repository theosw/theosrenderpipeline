#pragma once

#include <cstdint>
#include <optional>

namespace TheosRenderPipeline
{
    // Scene cuts (a load, the map, character editing) arrive as menu events and
    // SKSE messages, and each starts a new scene generation. Every temporal
    // consumer owes the newest generation one history reset on its first world
    // frame and settles it only when that frame succeeds. Loading-screen frames,
    // failed frames and other consumers cannot use the reset up, and the world
    // is not held in reset for a fixed number of frames.
    class SceneResetTracker
    {
    public:
        struct Settlement
        {
            std::uint32_t generation{};
            unsigned resetFrames{}, deferredFrames{};
        };

        // Call as a frame starts. Returns whether it must discard history.
        bool Begin(std::uint32_t generation, bool worldFrame)
        {
            frame_ = generation;
            owed_ = worldFrame && generation != settled_;
            if (owed_) { ++resetFrames_; }
            else if (generation != settled_) { ++deferredFrames_; }
            return owed_;
        }

        // Call when the frame Begin started has completed. A request that
        // arrived during the frame stays owed to the next one.
        std::optional<Settlement> Succeeded()
        {
            if (!owed_) { return std::nullopt; }
            owed_ = false;
            settled_ = frame_;
            Settlement result{frame_, resetFrames_, deferredFrames_};
            resetFrames_ = deferredFrames_ = 0;
            return result;
        }

    private:
        std::uint32_t settled_{}, frame_{};
        unsigned resetFrames_{}, deferredFrames_{};
        bool owed_{};
    };
}
