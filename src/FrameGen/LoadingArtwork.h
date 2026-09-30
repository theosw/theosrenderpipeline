#pragma once

#include <cstdint>

namespace TheosRenderPipeline::LoadingArtwork
{
    void Install();
    // Called by the render owner, independently of capture requests or budgets.
    void Boundary(std::uint64_t frame, bool world, bool mainMenu, bool loadingMenu) noexcept;
    void ResetAfterRetirement() noexcept;
    // True once after TRP asked the game to show artwork for a transition that
    // would otherwise only fade; the render owner fades that loading screen in.
    bool TakeForcedTransition() noexcept;
}
