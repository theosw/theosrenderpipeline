#pragma once
#include <cstdint>

namespace TheosRenderPipeline
{
    enum class FrameGenerationProvider : int { NVIDIA = 0, XeFG = 1 };
    constexpr bool ValidProvider(int value) { return value == 0 || value == 1; }
    constexpr const char* ProviderName(FrameGenerationProvider provider)
    {
        return provider == FrameGenerationProvider::XeFG ? "XeFG" : "DLSS-G";
    }
    struct ProviderStartup {
        FrameGenerationProvider requested;
        FrameGenerationProvider presenter;
    };
    constexpr ProviderStartup SelectProviderStartup(bool xeSS, bool nvidiaAdapter, FrameGenerationProvider requested)
    {
        if (!ValidProvider(static_cast<int>(requested))) { requested = FrameGenerationProvider::NVIDIA; }
        if (!nvidiaAdapter) { requested = FrameGenerationProvider::XeFG; }
        // XeSS can start either provider directly on NVIDIA. The existing
        // DLSS startup bridge stays NVIDIA until world inputs admit a switch.
        return {requested, xeSS ? requested : FrameGenerationProvider::NVIDIA};
    }
}
