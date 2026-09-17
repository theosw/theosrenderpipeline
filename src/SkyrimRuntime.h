#pragma once

#include <REL/Version.h>
#include <array>

#include "FrameGen/LoadingArtworkPolicy.h"

namespace TheosRenderPipeline::SkyrimRuntime
{
    struct Profile
    {
        REL::Version version;
        LoadingArtwork::TransitionCallers loadingArtwork;
    };

    // Admit a runtime only after its hook instructions and engine layouts have
    // been verified. Address Library coverage alone is not sufficient.
    inline constexpr std::array kProfiles{
        Profile{ REL::Version{ 1, 5, 97, 0 }, LoadingArtwork::kSECallers },
        Profile{ REL::Version{ 1, 6, 640, 0 }, LoadingArtwork::kAE640Callers },
        Profile{ REL::Version{ 1, 6, 1170, 0 }, LoadingArtwork::kAECallers },
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
