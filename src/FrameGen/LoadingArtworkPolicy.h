#pragma once

#include <array>
#include <cstdint>

namespace TheosRenderPipeline::LoadingArtwork
{
    struct TransitionCallers
    {
        std::uintptr_t exterior;
        std::uintptr_t interior;
    };

    inline constexpr TransitionCallers kSECallers{ 0x69C9A5, 0x69C9DD };
    inline constexpr TransitionCallers kAE640Callers{ 0x6D6AE0, 0x6D6B19 };
    inline constexpr TransitionCallers kAECallers{ 0x7309F0, 0x730A29 };
    inline constexpr TransitionCallers kAE17104Callers{ 0x743590, 0x7435C9 };
    inline constexpr TransitionCallers kGOG1179Callers{ 0x732C20, 0x732C59 };

    // The engine's transition sender as this hook expects to find it.
    inline constexpr std::array<std::uint8_t, 19> kTransitionPrologue{
        0x40,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x38,0xFE,0xFF,0xFF,0xFF};

    // Why the hook must not be installed, or null. Checked before anything is written.
    constexpr const char* InstallRefusal(bool profileKnown, bool prologueMatches) noexcept
    {
        return !profileKnown ? "No verified loading artwork profile exists for this Skyrim runtime" :
            !prologueMatches ? "Loading artwork hook does not match the engine transition sender" : nullptr;
    }

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
