#pragma once

// Preserve the existing TheosRenderPipeline INI mode values.
enum UpscaleType
{
    DLSS = 0,
    DLAA = 3,
    XeSS = 4,
    FSR = 5,  // FSR 3.1
    FSR4 = 6
};

inline constexpr bool IsFSR(int mode) { return mode == FSR || mode == FSR4; }

inline constexpr bool UsesNvidiaUpscaler(int mode) { return mode == DLSS || mode == DLAA; }
