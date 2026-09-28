#pragma once

#include "NeuralRenderingReconstruction.h"
#include "NeuralRenderingPassSettings.h"
#include "NeuralCombatPolicy.h"
#include "SourceDLSSGNeuralTelemetry.h"
#include <algorithm>
#include <filesystem>
#include <string>

namespace TheosRenderPipeline::SourceDLSSG
{
	struct NeuralOptions
	{
		bool enabled{ false }; // Standard DLSS unless source NR was explicitly saved.
		bool beforeUpscaling{ false };
		// Input contract supplied by the renderer adapter, never a saved setting.
		// CS can run either placement before UI composition.
		bool worldOnly{ false };
		bool WorldOnly() const { return beforeUpscaling || worldOnly; }
		int passes{ 1 };
		NeuralRendering::CombatSettings combat{};
		// Frame-only override: passes still describes the requested resource allocation.
		NeuralRendering::PassOverride passOverride{NeuralRendering::PassOverride::None};
		int EffectivePasses() const { return passes == 2 && passOverride != NeuralRendering::PassOverride::None ? 1 : passes; }
		std::filesystem::path runtimePath;
		NeuralRendering::Tuning tuning{};
		NeuralRendering::Reconstruction reconstruction{};
		NeuralRendering::SecondPassSettings secondPass{};
		NeuralRendering::SecondPassSettings EffectiveSecond() const
		{
			auto result = NeuralRendering::EffectiveSecondPass(secondPass, reconstruction, tuning);
			if (WorldOnly()) { result.tuning.uiCorrection = false; }
			return result;
		}
		bool operator==(const NeuralOptions&) const = default;
	};

	inline bool NeuralRuntimePresent(const std::filesystem::path& path)
	{
		std::error_code error;
		return !path.empty() && std::filesystem::is_regular_file(path, error) && !error;
	}

	inline NeuralOptions SanitizeNeuralOptions(NeuralOptions options)
	{
		options.tuning = NeuralRendering::SanitizeBuild14Tuning(options.tuning);
		options.passes = std::clamp(options.passes, 1, 2);
		options.combat = NeuralRendering::SanitizeCombatSettings(options.combat);
		options.passOverride = NeuralRendering::PassOverride::None;
		options.secondPass = NeuralRendering::SanitizeSecondPass(options.secondPass);
		options.reconstruction = NeuralRendering::SanitizeReconstruction(options.reconstruction);
		// Reject a missing optional runtime before any GPU work is recorded.
		// Runtime identity and initialization checks still belong to the session.
		options.enabled = options.enabled && NeuralRuntimePresent(options.runtimePath);
		return options;
	}

	struct NeuralSnapshot
	{
		bool active{ false }, failed{ false };
		int effectivePasses{1};
		NeuralRendering::PassOverride passOverride{NeuralRendering::PassOverride::None};
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
			auto historyOptions = options;
			// Trigger/recovery labels and policy edits alone do not change the image.
			historyOptions.combat = {};
			historyOptions.passOverride = options.EffectivePasses() != options.passes ?
				NeuralRendering::PassOverride::Combat : NeuralRendering::PassOverride::None;
			const bool reset = cameraReset || active != active_ || (active && historyOptions != previous_);
			active_ = active;
			previous_ = historyOptions;
			return reset;
		}
		void Invalidate() { active_ = false; }
	private:
		bool active_{};
		NeuralOptions previous_;
	};

}
