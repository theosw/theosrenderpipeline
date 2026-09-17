#include "SourceDLSSGBackend.h"

namespace TheosRenderPipeline::SourceDLSSG
{
    // Shared settings remain saved for the add-on. The base never creates an NR session.
    void Backend::ConfigureNeuralRendering(NeuralOptions) {}
    NeuralOptions Backend::NeuralConfiguration() const { return {}; }
    NeuralSnapshot Backend::NeuralState() const { return {}; }
    bool Backend::EvaluateNeuralBeforeUpscaling(const NeuralOptions&, const sl::Constants*,
        bool, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*, FrameExtent, bool&)
    {
        return Ready();
    }
    bool Backend::EvaluateNeuralWorld(const NeuralOptions&, const sl::Constants*, bool,
        ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*, FrameExtent, FrameExtent, bool&)
    {
        return Ready();
    }
}
