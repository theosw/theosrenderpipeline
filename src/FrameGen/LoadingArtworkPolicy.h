#pragma once

#include <cstdint>

namespace TheosRenderPipeline::LoadingArtwork
{
    constexpr std::uint8_t Suppression(bool enabled, bool ready, bool gameCaller,
        std::uintptr_t callerRVA, std::uint8_t show, std::uint8_t suppress,
        std::uint8_t immediate) noexcept
    {
        // Only the queued exterior/interior Show requests also carry this flag
        // into the immediate Update that latches the artwork-selection decision.
        if (enabled && ready && gameCaller && show == 1 && suppress == 1 && immediate == 1 &&
            (callerRVA == 0x7309F0 || callerRVA == 0x730A29)) {
            return 0;
        }
        return suppress;
    }
}
