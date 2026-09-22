#pragma once

#include "SourceDLSSGNeuralState.h"
#include "NeuralRenderingRuntimeContract.h"

namespace TheosRenderPipeline::SourceDLSSG
{
    // Probe at an NR request boundary, before opening a command list or creating
    // a feature. Stable frames and reconstruction changes reuse this identity.
    class NeuralRuntimeAvailability
    {
    public:
        void Reset() { checked_ = false; }

        template<class Classify> const char* UnavailableReason(const NeuralOptions& options, Classify classify)
        {
            if (!options.enabled) { return nullptr; }
            if (!checked_ || path_ != options.runtimePath) {
                build_ = classify(options.runtimePath);
                path_ = options.runtimePath;
                checked_ = true;
            }
            if (build_ == NeuralRendering::RuntimeBuild::Unknown) {
                return "NR runtime is unsupported; NR skipped, DLSS and frame generation remain available";
            }
            if (!NeuralRendering::UsesReconstructionContract(build_) &&
                NeuralRendering::EffectiveResolve(options.reconstruction) != NeuralRendering::ResolveMethod::Auto) {
                return options.reconstruction.producerColor ?
                    "This NR runtime cannot run before CS upscaling; use After upscaling, Auto, 100% input and peripheral compression off" :
                    "This NR runtime requires Auto reconstruction at 100% input with peripheral compression off; NR skipped";
            }
            if (!NeuralRendering::UsesReconstructionContract(build_) && options.passes == 2 &&
                options.EffectiveSecond().inputScale != NeuralRendering::NormalizeInputScale(options.reconstruction.inputScale)) {
                return "Independent NR pass resolutions require the newer NR runtime; use linked settings or equal resolutions; NR skipped";
            }
            return nullptr;
        }

    private:
        bool checked_{};
        std::filesystem::path path_;
        NeuralRendering::RuntimeBuild build_{NeuralRendering::RuntimeBuild::Unknown};
    };
}
