#pragma once
#include "FrameGen/NeuralObjectCapture.h"
#include <atomic>

namespace TheosRenderPipeline::NeuralObjects
{
    void Install();
    void Configure(bool enabled);
    bool Active();
    void ResetAfterRetirement();
    void RecordDraw(ID3D11DeviceContext* context, const std::function<void()>& draw);
    ID3D11Texture2D* Current();
    const char* Status();
    // Whether object rules can work: Skyrim 1.5.97/1.6.1170 and no conflicting patch. The
    // hook itself installs only when a group is first switched on.
    bool Available();
}
