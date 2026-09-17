#pragma once

#include <REL/Version.h>
#include <array>

#include "FrameGen/LoadingArtworkPolicy.h"

namespace TheosRenderPipeline::SkyrimRuntime
{
    struct HookOffsets
    {
        std::uintptr_t mistBackground;
        std::uintptr_t rendererClientRect;
        std::uintptr_t updateJitter;
        std::uintptr_t cameraBranch;
        std::uintptr_t renderWorld;
    };

    struct GraphicsLayout
    {
        std::uintptr_t runtimeData;
        std::uintptr_t frameCounter;
    };

    inline constexpr HookOffsets kSEHooks{ 0x7A1, 0x192, 0xE5, 0x1D5, 0x831 };
    inline constexpr HookOffsets kAEHooks{ 0x7A4, 0x18B, 0xE2, 0x1D5, 0x841 };
    inline constexpr HookOffsets kAE17104Hooks{ 0x7B9, 0x1DC, 0x133, 0x1D6, 0x85E };

    struct Profile
    {
        REL::Version version;
        LoadingArtwork::TransitionCallers loadingArtwork;
        HookOffsets hooks;
        GraphicsLayout graphics;
    };

    // Admit a runtime only after its hook instructions and engine layouts have
    // been verified. Address Library coverage alone is not sufficient.
    inline constexpr std::array kProfiles{
        Profile{ REL::Version{ 1, 5, 97, 0 }, LoadingArtwork::kSECallers, kSEHooks, { 0x58, 0x4C } },
        Profile{ REL::Version{ 1, 6, 640, 0 }, LoadingArtwork::kAE640Callers, kAEHooks, { 0x60, 0x4C } },
        Profile{ REL::Version{ 1, 6, 1170, 0 }, LoadingArtwork::kAECallers, kAEHooks, { 0x60, 0x4C } },
        Profile{ REL::Version{ 1, 7, 104, 0 }, LoadingArtwork::kAE17104Callers, kAE17104Hooks, { 0x70, 0x54 } },
    };

    [[nodiscard]] constexpr const Profile* Find(REL::Version version) noexcept
    {
        for (const auto& profile : kProfiles) {
            if (profile.version == version) {
                return &profile;
            }
        }
        return nullptr;
    }
}
