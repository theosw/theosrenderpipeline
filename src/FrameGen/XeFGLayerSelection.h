#pragma once

namespace TheosRenderPipeline::SourceDLSSG
{
    // Which images XeFG may interpolate from this frame. The HDR output pass's
    // HUD-less and UI targets are written only when it composes a prepared frame
    // with both layers; on any other frame they still hold an older image, so
    // they are offered only while HDR output is signalled and was composed now.
    enum class XeFGLayer { None, Standard, HDR };
    struct XeFGLayers
    {
        XeFGLayer hudless{ XeFGLayer::None };
        XeFGLayer ui{ XeFGLayer::None };
    };

    constexpr XeFGLayers SelectXeFGLayers(bool hdrSignalled, bool hdrPassReady, bool hdrComposedThisFrame, bool uiSource)
    {
        if (hdrSignalled && hdrPassReady) {
            // Never the stale HDR world image; without a fresh one, no generation.
            return hdrComposedThisFrame ? XeFGLayers{ XeFGLayer::HDR, XeFGLayer::HDR } : XeFGLayers{};
        }
        return { XeFGLayer::Standard, uiSource ? XeFGLayer::Standard : XeFGLayer::None };
    }
}
