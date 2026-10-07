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
    // xeSS: TRP runs the XeSS upscaler. It is false on the Community Shaders
    // route, where CS upscales whatever TRP's UpscaleType says.
    constexpr ProviderStartup SelectProviderStartup(bool xeSS, bool nvidiaAdapter, FrameGenerationProvider requested)
    {
        if (!ValidProvider(static_cast<int>(requested))) { requested = FrameGenerationProvider::NVIDIA; }
        // Without an NVIDIA adapter only XeFG can present, whoever upscales.
        if (!nvidiaAdapter) { return {FrameGenerationProvider::XeFG, FrameGenerationProvider::XeFG}; }
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
