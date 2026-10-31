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
    constexpr FrameGenerationProvider NormalizeProviderRequest(FrameGenerationProvider requested, bool nvidiaAvailable)
    {
        if (!nvidiaAvailable) { return FrameGenerationProvider::XeFG; }
        return ValidProvider(static_cast<int>(requested)) ? requested : FrameGenerationProvider::NVIDIA;
    }
    // Skyrim's native projection is forward Z. An external upscaler may replace
    // it; keep the existing CS startup convention until its first valid camera.
    constexpr bool XeFGStartupInvertedDepth(bool externalUpscaler) { return externalUpscaler; }
    // xeSS: TRP runs the XeSS upscaler. It is false on the Community Shaders
    // route, where CS upscales whatever TRP's UpscaleType says.
    constexpr ProviderStartup SelectProviderStartup(bool xeSS, bool nvidiaAvailable, FrameGenerationProvider requested)
    {
        requested = NormalizeProviderRequest(requested, nvidiaAvailable);
        // Unsupported NVIDIA devices (including research Pascal) also use XeFG.
        if (!nvidiaAvailable) { return {requested, requested}; }
        // XeSS can start either provider directly on NVIDIA. The existing
        // DLSS startup bridge stays NVIDIA until world inputs admit a switch.
        return {requested, xeSS ? requested : FrameGenerationProvider::NVIDIA};
    }
    // A live switch to NVIDIA initializes Streamline. In an XeSS-started session
    // NR may already have initialized NGX through the driver loader; Streamline's
    // DLSS-G plugin then never loads nvngx_dlssg.dll, and the MFG verification
    // that follows is fatal. Refuse that switch until restart.
    constexpr bool NvidiaSwitchNeedsRestart(bool streamlineInitialized, bool ngxInitializedByNR)
    {
        return !streamlineInitialized && ngxInitializedByNR;
    }
}
