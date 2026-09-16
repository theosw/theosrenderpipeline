#pragma once

#include "NeuralRenderingReconstruction.h"
#include "NeuralRenderingTuning.h"
#include "SourceDLSSGNeuralTelemetry.h"
#include <filesystem>
#include <string>

namespace TheosRenderPipeline::SourceDLSSG
{
	struct NeuralOptions
	{
		bool enabled{ false }; // Standard DLSS unless source NR was explicitly saved.
		bool beforeUpscaling{ false };
		int passes{ 1 };
		std::filesystem::path runtimePath;
		NeuralRendering::Tuning tuning{};
		NeuralRendering::Reconstruction reconstruction{};
		bool operator==(const NeuralOptions&) const = default;
	};

	struct NeuralSnapshot
	{
		bool active{ false }, failed{ false };
		std::uint64_t evaluations{}, resets{};
		NeuralTelemetrySnapshot telemetry{};
		std::string status{ "standard DLSS; source NR is off" };
	};

	// A frame decision, not a generation policy. NR continues when FG is off.
	// No-input/loading frames break history. Options are sampled once per frame.
	class NeuralHistory
	{
	public:
		bool ResetFor(const NeuralOptions& options, bool eligible, bool cameraReset)
		{
			const bool active = eligible && options.enabled;
			const bool reset = cameraReset || active != active_ || (active && options != previous_);
			active_ = active;
			previous_ = options;
			return reset;
		}
		void Invalidate() { active_ = false; }
	private:
		bool active_{};
		NeuralOptions previous_;
	};

}
