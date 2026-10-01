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
}
