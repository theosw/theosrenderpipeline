#pragma once
#include <cstdint>

namespace TheosRenderPipeline::NeuralRendering
{
    struct SubrectKeys { const char* x; const char* y; const char* width; const char* height; };
    namespace Subrect
    {
#define TRP_SUBRECT_KEYS(name) inline constexpr SubrectKeys name{ \
        "DLSSNR." #name "SubrectBaseX", "DLSSNR." #name "SubrectBaseY", \
        "DLSSNR." #name "SubrectWidth", "DLSSNR." #name "SubrectHeight" }
        TRP_SUBRECT_KEYS(Color);
        TRP_SUBRECT_KEYS(MVec);
        TRP_SUBRECT_KEYS(Depth);
        TRP_SUBRECT_KEYS(Output);
        TRP_SUBRECT_KEYS(Backbuffer);
        TRP_SUBRECT_KEYS(ControlMask);
        TRP_SUBRECT_KEYS(UI);
        TRP_SUBRECT_KEYS(UIAlpha);
        TRP_SUBRECT_KEYS(BidirectionalDistortionField);
#undef TRP_SUBRECT_KEYS
    }

    template<class Parameters>
    void SetSubrect(Parameters* parameters, const SubrectKeys& keys,
        std::uint32_t width, std::uint32_t height)
    {
        // Keep all writes: the vendor parameter object may be reused or changed
        // by evaluation. Only construction of the invariant names is removed.
        parameters->Set(keys.x, 0u);
        parameters->Set(keys.y, 0u);
        parameters->Set(keys.width, width);
        parameters->Set(keys.height, height);
    }
}
