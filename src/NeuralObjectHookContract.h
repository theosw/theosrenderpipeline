#pragma once
#include <RE/Skyrim.h>
#include <array>
#include <cstdint>

namespace TheosRenderPipeline::NeuralObjects::DrawHook
{
    struct Contract
    {
        std::uint64_t relocationID;
        std::array<std::uint8_t, 19> prefix;
    };
    // Verified against exact-version offline images and their draw callers.
    // Both entries take (BSRenderPass*, uint32_t, bool, uint32_t), with no this.
    inline constexpr Contract kSE{100854, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,
        0x56,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20}};
    inline constexpr Contract kAE{107644, {0x48,0x8b,0xc4,0x57,0x41,0x54,0x41,0x55,
        0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x80,0x00,0x00,0x00}};

    constexpr const Contract* ForVersion(REL::Version version)
    {
        if (version == REL::Version{1,5,97,0}) { return &kSE; }
        if (version == REL::Version{1,6,1170,0}) { return &kAE; }
        return nullptr;
    }
}
