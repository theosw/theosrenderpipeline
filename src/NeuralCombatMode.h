#pragma once

#include "FrameGen/SourceDLSSGNeuralState.h"

namespace TheosRenderPipeline::NeuralRendering
{
    // Called once at the selected producer's world-frame boundary, on its render thread.
    // Actor state is sampled by an SKSE main-thread task; no actor pointer crosses threads.
    void ApplyCombatMode(SourceDLSSG::NeuralOptions& options, bool worldEligible);
}
