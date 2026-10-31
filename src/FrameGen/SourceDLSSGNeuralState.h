#pragma once

#include "NeuralRenderingReconstruction.h"
#include "NeuralRenderingPassSettings.h"
#include "NeuralCombatPolicy.h"
#include "NeuralObjectMask.h"
#include "NeuralRenderingCompare.h"
#include "NeuralRuntimeFiles.h"
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
		// Frame-only: a preset runs this many passes while passes stay allocated;
		// zero runs them all.
		int presetPasses{};
		int EffectivePasses() const
		{
			const int requested = presetPasses > 0 ? (std::min)(presetPasses, passes) : passes;
			// Combat, weapons and their recovery run Pass 1 only.
			const bool onePass = passOverride != NeuralRendering::PassOverride::None &&
				passOverride != NeuralRendering::PassOverride::Preset;
			return onePass ? 1 : requested;
		}
		std::filesystem::path runtimePath;
		// The backend replaces runtimePath with the best bundled runtime for the GPU.
		bool automaticRuntime{};
		NeuralRendering::Tuning tuning{};
        NeuralRendering::ObjectMaskSettings objectMasks{};
		NeuralRendering::Reconstruction reconstruction{};
		// Passes 2 to 5; index 0 is Pass 2.
		NeuralRendering::ExtraPasses extraPasses{};
		// Session-only, set live from the menu; the backend adds it per frame.
		NeuralRendering::CompareSettings compare{};
		// Per-frame appearance controller revision; zero preserves manual-edit resets.
		std::uint64_t appearanceRevision{};
		// An extra pass (index 0 is Pass 2), with Pass 1's settings while linked.
		NeuralRendering::ExtraPassSettings EffectiveExtra(std::size_t index) const
		{
			auto result = NeuralRendering::EffectivePass(extraPasses[index], reconstruction, tuning);
			if (WorldOnly()) { result.tuning.uiCorrection = false; }
			return result;
		}
		bool operator==(const NeuralOptions&) const = default;
	};

    inline NeuralOptions NormalizeNeuralRoute(NeuralOptions options, bool communityShaders)
    {
        options.worldOnly = communityShaders;
        if (communityShaders) {
            options.tuning.uiCorrection = false;
            options.reconstruction.producerColor = options.beforeUpscaling;
        }
        return options;
    }

	inline NeuralOptions SanitizeNeuralOptions(NeuralOptions options)
	{
		options.tuning = NeuralRendering::SanitizeBuild14Tuning(options.tuning);
        options.objectMasks = NeuralRendering::SanitizeObjectMask(options.objectMasks);
		options.passes = std::clamp(options.passes, 1, NeuralRendering::kMaxNeuralPasses);
		options.combat = NeuralRendering::SanitizeCombatSettings(options.combat);
		options.passOverride = NeuralRendering::PassOverride::None;
		options.extraPasses = NeuralRendering::SanitizeExtraPasses(options.extraPasses);
		options.reconstruction = NeuralRendering::SanitizeReconstruction(options.reconstruction);
		options.compare = NeuralRendering::SanitizeCompare(options.compare);
		// Automatic is checked by GPU-aware candidate selection after sanitizing.
		// Explicit paths still fail before any GPU work is recorded.
		// Runtime identity and initialization checks still belong to the session.
		options.enabled = options.enabled && (options.automaticRuntime || NeuralRuntimePresent(options.runtimePath));
		return options;
	}

	struct NeuralSnapshot
	{
		bool active{ false }, failed{ false };
		// Post-admission status only; requested/saved options are never masked.
		bool sessionDisabled{}, retryable{};
		std::string reason;
		int effectivePasses{1};
		NeuralRendering::PassOverride passOverride{NeuralRendering::PassOverride::None};
		std::uint64_t evaluations{}, resets{};
		NeuralTelemetrySnapshot telemetry{};
		std::string status{ "standard DLSS; source NR is off" };
		// Set when a runtime known not to work on this GPU was replaced; kept for the session.
		std::string fallback;
		// Set while a later pass could not start and fewer passes run than requested.
		std::string passNote;
	};

	inline bool SameHistoryOptions(NeuralOptions current, const NeuralOptions& previous)
	{
		auto previousComparable=previous;
		current.objectMasks.previewGroups=previousComparable.objectMasks.previewGroups=0;
		current.objectMasks.previewClassesOnly=previousComparable.objectMasks.previewClassesOnly=false;
		current.compare=previousComparable.compare={};
		current.reconstruction.broadStrength=previousComparable.reconstruction.broadStrength=1;
		current.reconstruction.detailStrength=previousComparable.reconstruction.detailStrength=1;
		// The model rate changes when the model runs, not what its history holds.
		current.reconstruction.modelRate=previousComparable.reconstruction.modelRate={};
		if (current.appearanceRevision && current.appearanceRevision == previous.appearanceRevision) {
			const auto retain = [](NeuralRendering::Tuning& value, const NeuralRendering::Tuning& old) {
				value.intensity = old.intensity;
				value.localToneStrength = old.localToneStrength;
				value.localStructureStrength = old.localStructureStrength;
			};
			retain(current.tuning, previous.tuning);
			for (std::size_t i = 0; i < current.extraPasses.size(); ++i) {
				retain(current.extraPasses[i].tuning, previous.extraPasses[i].tuning);
			}
		}
		return current == previousComparable;
	}

	// Model rate: whether NR's change stored on the previous frame still stands
	// for this one. Anything that changes NR's output does not, except the look
	// settings weather and time of day blend each frame under one appearance
	// revision. The comparison and mask preview are drawn after the change is
	// stored, and only how many passes run matters, not why.
	inline bool SameModelOutputOptions(NeuralOptions current, NeuralOptions previous)
	{
		for (auto* options : { &current, &previous }) {
			options->presetPasses = options->EffectivePasses();
			options->passOverride = NeuralRendering::PassOverride::None;
			options->combat = {};
		}
		const bool blending = current.appearanceRevision && current.appearanceRevision == previous.appearanceRevision;
		const bool bands = current.reconstruction.broadStrength == previous.reconstruction.broadStrength &&
			current.reconstruction.detailStrength == previous.reconstruction.detailStrength;
		return SameHistoryOptions(current, previous) && (blending || bands);
	}

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
			// Only how many passes run changes the image, not why.
			historyOptions.passOverride = NeuralRendering::PassOverride::None;
			historyOptions.presetPasses = options.EffectivePasses() != options.passes ? options.EffectivePasses() : 0;
			// Model-only changes are retired by NeuralPass, which resets just the
			// changed pass and its downstream histories. Do not reset upstream here.
			historyOptions.reconstruction.preset = 0;
			for (auto& extra : historyOptions.extraPasses) { extra.preset = 0; }
			const bool reset = cameraReset || active != active_ || (active && !SameHistoryOptions(historyOptions, previous_));
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
