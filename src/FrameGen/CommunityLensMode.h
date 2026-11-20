#pragma once

namespace TheosRenderPipeline
{
    // Community Shaders route: where Bottled Shaders' Camera FX lens overlay
    // (rain, frost, blood on the lens) is drawn while TRP generates frames,
    // and so how frame generation sees it. Saved as [CommunityShaders] LensEffects.
    enum class CommunityLensMode : int
    {
        Scene,   // On the scene, as Bottled draws it without its own frame generation: interpolated with the world.
        WithHUD, // In a layer under the HUD and out of the HUD-less scene: frame generation sees it as it sees the HUD.
    };

    constexpr CommunityLensMode SanitizeCommunityLens(long value)
    {
        return value == static_cast<long>(CommunityLensMode::Scene) ? CommunityLensMode::Scene : CommunityLensMode::WithHUD;
    }
}
