#pragma once

namespace TheosRenderPipeline::ModexMenuScope
{
    // Modex draws its ImGui menu and 3D item preview from its menu's PostDisplay.
    // The preview copies to and from the game-facing surface directly, in native
    // screen coordinates, so those copies need the native UI target during the
    // draw. Called when the Modex menu opens; wraps PostDisplay once per class.
    void OnMenuOpened();

    // True on the drawing thread while Modex's PostDisplay is running.
    bool DisplayActive();
}
