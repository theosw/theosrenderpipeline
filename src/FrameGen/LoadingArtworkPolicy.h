#pragma once

#include <cstdint>

namespace TheosRenderPipeline::LoadingArtwork
{
    struct TransitionCallers
    {
        std::uintptr_t exterior;
        std::uintptr_t interior;
    };

    inline constexpr TransitionCallers kSECallers{ 0x69C9A5, 0x69C9DD };
    inline constexpr TransitionCallers kAECallers{ 0x7309F0, 0x730A29 };

    constexpr std::uint8_t Suppression(TransitionCallers callers, bool enabled, bool ready, bool gameCaller,
        std::uintptr_t callerRVA, std::uint8_t show, std::uint8_t suppress,
        std::uint8_t immediate) noexcept
    {
        // Only the queued exterior/interior Show requests also carry this flag
        // into the immediate Update that latches the artwork-selection decision.
        if (enabled && ready && gameCaller && show == 1 && suppress == 1 && immediate == 1 &&
            (callerRVA == callers.exterior || callerRVA == callers.interior)) {
            return 0;
        }
        return suppress;
    }
}
