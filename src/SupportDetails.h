#pragma once
#include "OverlayUI.h"
#include "SupportSummary.h"

namespace TheosRenderPipeline::Support
{
    // Fills what the menu knows (what runs now, NVIDIA App state, degraded
    // reasons) into a summary that already holds the system part.
    void AddMenuState(Summary& summary, const OverlayUI::FrameView& view);
}
