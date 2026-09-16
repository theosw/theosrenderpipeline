#pragma once
#include "SolFGLateOverlayAPI.h"

// Optional additive API, discovered as SolFG_GetStartupOverlayBridge. The V1
// wire layout is shared with the late-overlay API, but this factory promises a
// transparent native-resolution foreground layer, composed AFTER reconstruction.
// It does not expose an already-upscaled scene. Only use it around a bounded
// original ImGui renderer call; pair every successful begin with end on the
// rendering thread. Query/begin return zero outside the supported startup phase.
// Existing SolFG_GetLateOverlayBridge semantics and binary layout are unchanged.
namespace SolFGStartupOverlayAPI
{
    using FrameV1 = SolFGLateOverlayAPI::FrameV1;
    using BridgeV1 = SolFGLateOverlayAPI::BridgeV1;
    using GetBridge = SolFGLateOverlayAPI::GetBridge;
    inline constexpr auto kVersion1 = SolFGLateOverlayAPI::kVersion1;
}
