#pragma once
#include <d3d12.h>
#include <memory>
#include <string>

namespace TheosRenderPipeline::NeuralRendering
{
    // Experimental, pinned-runtime instrumentation. Each instance belongs to one
    // feature generation. The host submits all evaluations on the same queue and
    // drains it before destroying/recreating the feature.
    class BottleneckReuse final
    {
    public:
        BottleneckReuse();
        ~BottleneckReuse();
        BottleneckReuse(const BottleneckReuse&) = delete;
        BottleneckReuse& operator=(const BottleneckReuse&) = delete;
        static bool Install(HMODULE verifiedRuntime);
        bool Begin(ID3D12GraphicsCommandList* list, bool reset);
        // False means a partially filtered list MUST NOT be submitted.
        bool End(bool evaluationSucceeded);
        void Submitted();
        bool Reused() const;
        const std::string& Status() const;
    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
