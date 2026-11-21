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

	inline float SanitizeIntensity(float value)
	{
		return std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 1.0f;
	}

	inline Tuning SanitizeTuning(Tuning a_tuning)
	{
		const auto unit = [](float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f; };
		a_tuning.style = std::clamp(a_tuning.style, 0, 2);
		a_tuning.intensity = SanitizeIntensity(a_tuning.intensity);
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
		// The runtime takes any style. On 310.8 and 310.9.16 every value from 2
		// up gives Cinematic's exact output, but a later runtime may add styles.
		value.style = std::clamp(value.style, 0, 255);
		value.intensity = SanitizeIntensity(value.intensity);
		value.localToneStrength = strength(value.localToneStrength);
		value.localStructureStrength = strength(value.localStructureStrength);
		value.skinStructureStrength = strength(value.skinStructureStrength, -1.0f);
		return value;
	}
}
