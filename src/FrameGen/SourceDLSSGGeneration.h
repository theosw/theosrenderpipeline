#pragma once
#include <algorithm>
#include <cstdint>

namespace TheosRenderPipeline::SourceDLSSG
{
	struct GenerationRequest
	{
		std::uint32_t generatedFrames{ 1 };
		std::uint32_t dynamicTargetFPS{ 0 }; // Zero lets the runtime use display refresh.
		bool dynamic{ false };
		bool operator==(const GenerationRequest&) const = default;
	};
	constexpr bool ValidDynamicTarget(std::uint32_t target)
	{
		return target == 0 || (target > 60 && target <= 1000);
	}
	constexpr bool ValidGenerationRequest(const GenerationRequest& value)
	{
		return value.generatedFrames >= 1 && value.generatedFrames <= 5 &&
			(!value.dynamic || ValidDynamicTarget(value.dynamicTargetFPS));
	}
	constexpr GenerationRequest SanitizeGenerationRequest(GenerationRequest value)
	{
		if (value.generatedFrames < 1 || value.generatedFrames > 5) { value.generatedFrames = 1; }
		if (!ValidDynamicTarget(value.dynamicTargetFPS)) { value.dynamicTargetFPS = 0; }
		return value;
	}
	struct GenerationSelection
	{
		GenerationRequest effective;
		bool limited{};
	};
	constexpr GenerationSelection SelectGeneration(GenerationRequest requested, std::uint32_t maximum, bool dynamicSupported)
	{
		GenerationSelection result{ requested };
		result.effective.generatedFrames = (std::min)(requested.generatedFrames, (std::max)(1u, maximum));
		result.effective.dynamic = requested.dynamic && dynamicSupported && maximum > 1;
		if (!result.effective.dynamic) { result.effective.dynamicTargetFPS = 0; }
		result.limited = maximum == 0 || result.effective.generatedFrames != requested.generatedFrames ||
			result.effective.dynamic != requested.dynamic;
		return result;
	}
}
