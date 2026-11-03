#pragma once

// Preserve the existing TheosRenderPipeline INI mode values.
enum UpscaleType
{
    DLSS = 0,
    DLAA = 3,
    XeSS = 4,
    FSR = 5
};

inline constexpr bool UsesNvidiaUpscaler(int mode) { return mode == DLSS || mode == DLAA; }
