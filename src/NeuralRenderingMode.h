#pragma once

#include "UpscaleType.h"

namespace TheosRenderPipeline
{
// DLAA uses the same world-frame NR stages and native UI composition as DLSS.
// CS supplies its own world boundary independently of the saved TRP mode.
inline constexpr bool SupportsNeuralRenderingMode(int mode, bool externalWorld)
{
    return externalWorld || mode == DLSS || mode == DLAA;
}
}
