#pragma once

#include <cstdint>

namespace TheosRenderPipeline::LoadingArtwork
{
    void Install();
    // Called by the render owner, independently of capture requests or budgets.
    void Boundary(std::uint64_t frame, bool world, bool mainMenu, bool loadingMenu) noexcept;
    void ResetAfterRetirement() noexcept;
}
