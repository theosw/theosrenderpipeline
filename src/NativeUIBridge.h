#pragma once

#include <SolFGLateOverlayAPI.h>
#include <SolFGStartupOverlayAPI.h>

namespace TheosRenderPipeline::NativeUIBridge
{
    // Internal callers and the public exports share the same service tables.
    const SolFGLateOverlayAPI::BridgeV1* LateOverlay(std::uint32_t version);
    const SolFGStartupOverlayAPI::BridgeV1* Startup(std::uint32_t version);
}
