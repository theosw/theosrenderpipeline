#pragma once

#include <algorithm>
#include <cmath>

namespace TheosRenderPipeline::NeuralRendering
{
	// Evaluation controls shared by legacy and Build14 runtimes. Their accepted
	// ranges differ, so callers must choose the matching sanitizer.
	struct Tuning
	{
		int style{ 0 };
		float intensity{ 1.0f };
		float localToneStrength{ 1.0f };
		float localStructureStrength{ 1.0f };
		float skinStructureStrength{ 1.0f };
		bool useAutoSkinMask{ false };
		bool uiCorrection{ false };

		bool operator==(const Tuning&) const = default;
	};

	inline Tuning SanitizeTuning(Tuning a_tuning)
	{
		const auto unit = [](float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f; };
		a_tuning.style = std::clamp(a_tuning.style, 0, 2);
		a_tuning.intensity = unit(a_tuning.intensity);
		a_tuning.localToneStrength = unit(a_tuning.localToneStrength);
		a_tuning.localStructureStrength = unit(a_tuning.localStructureStrength);
		a_tuning.skinStructureStrength = unit(a_tuning.skinStructureStrength);
		return a_tuning;
	}

	inline Tuning SanitizeBuild14Tuning(Tuning value)
	{
		// The skin control accepts -1 as a runtime sentinel. Preserve it
		// rather than treating it as an ordinary negative gain.
		const auto strength = [](float input, float minimum = 0.0f) {
			return std::isfinite(input) ? std::clamp(input, minimum, 2.0f) : 1.0f;
		};
		value.style = std::clamp(value.style, 0, 7);
		value.intensity = strength(value.intensity);
		value.localToneStrength = strength(value.localToneStrength);
		value.localStructureStrength = strength(value.localStructureStrength);
		value.skinStructureStrength = strength(value.skinStructureStrength, -1.0f);
		return value;
	}
}
