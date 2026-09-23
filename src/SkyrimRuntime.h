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
        std::uintptr_t cursorBounds;
        std::uintptr_t screenSize;
        std::uintptr_t engineDimensions;
        std::uintptr_t worldCompletion;
        std::uintptr_t initD3D;
        std::uintptr_t drs;
        std::uintptr_t jitterBranch;
        std::uintptr_t csPostProcessing;
    };

    struct GraphicsLayout
    {
        std::uintptr_t runtimeData;
        std::uintptr_t frameCounter;
    };

    inline constexpr HookOffsets kSEHooks{ 0x7A1, 0x192, 0xE5, 0x1D5, 0x831,
        0xB, 0x102, 0x8E, 0x16F, 0x50, 0x2D, 0xE, 0x1F0 };
    inline constexpr HookOffsets kAEHooks{ 0x7A4, 0x18B, 0xE2, 0x1D5, 0x841,
        0xB, 0x102, 0x84, 0x17A, 0x2BC, 0x2D, 0x11, 0x1E7 };
    inline constexpr HookOffsets kAE17104Hooks{ 0x7B9, 0x1DC, 0x133, 0x1D6, 0x85E,
        0xB, 0x102, 0x84, 0x17A, 0x2BC, 0x2D, 0x11, 0x1E7 };

    struct HookBytes
    {
        std::array<std::uint8_t, 6> jitterBranch;
        std::array<std::uint8_t, 10> cameraBranch;
        std::array<std::uint8_t, 5> drawInterface;
    };
    // Original instructions verified against the identified executable images;
    // displacement bytes are part of the NOP contract, not wildcards.
    inline constexpr HookBytes kSEBytes{ {0x80,0x7A,0x18,0x00,0x74,0x7A},
        {0x80,0x79,0x18,0x00,0x0F,0x84,0xC0,0x00,0x00,0x00}, {0x40,0x53,0x55,0x56,0x57} };
    inline constexpr HookBytes kAEBytes{ {0x80,0x7A,0x18,0x00,0x74,0x68},
        {0x80,0x79,0x18,0x00,0x0F,0x84,0xC0,0x00,0x00,0x00}, {0x4C,0x8B,0xDC,0x56,0x57} };
    inline constexpr HookBytes kAE17104Bytes{ {0x80,0x7A,0x18,0x00,0x74,0x72},
        {0x80,0x79,0x18,0x00,0x0F,0x84,0xD4,0x00,0x00,0x00}, {0x4C,0x8B,0xDC,0x56,0x57} };

    struct Profile
    {
        REL::Version version;
        LoadingArtwork::TransitionCallers loadingArtwork;
        HookOffsets hooks;
        GraphicsLayout graphics;
        HookBytes bytes;
    };

    // Admit a runtime only after its hook instructions and engine layouts have
    // been verified. Address Library coverage alone is not sufficient.
    inline constexpr std::array kProfiles{
        Profile{ REL::Version{ 1, 5, 97, 0 }, LoadingArtwork::kSECallers, kSEHooks, { 0x58, 0x4C }, kSEBytes },
        Profile{ REL::Version{ 1, 6, 640, 0 }, LoadingArtwork::kAE640Callers, kAEHooks, { 0x60, 0x4C }, kAEBytes },
        Profile{ REL::Version{ 1, 6, 1170, 0 }, LoadingArtwork::kAECallers, kAEHooks, { 0x60, 0x4C }, kAEBytes },
        Profile{ REL::Version{ 1, 7, 104, 0 }, LoadingArtwork::kAE17104Callers, kAE17104Hooks, { 0x70, 0x54 }, kAE17104Bytes },
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
