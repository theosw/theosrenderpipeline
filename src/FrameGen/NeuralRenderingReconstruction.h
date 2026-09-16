#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace TheosRenderPipeline::NeuralRendering
{
	enum class ResolveMethod : std::uint32_t { Auto, Residual, Ratio };
	struct Reconstruction
	{
		int preset{ 0 };
		ResolveMethod method{ ResolveMethod::Auto };
		float inputScale{ 1 }, transferStrength{ 1 }, colourStrength{ 1 }, maxRatio{ 2 }, whitePoint{ 1 };
		bool colorIsHDR{};
		bool operator==(const Reconstruction&) const = default;
	};
	inline float NormalizeInputScale(float value)
	{
		// Zero, nonfinite and >=1 all mean native resolution.
		return value > 0 && value < 1 ? (std::max)(value, 0.25f) : 1.0f;
	}
	inline Reconstruction SanitizeReconstruction(Reconstruction value)
	{
		if (value.preset < 0 || value.preset > 1) { value.preset = 0; }
		if (value.method > ResolveMethod::Ratio) { value.method = ResolveMethod::Auto; }
		value.inputScale = NormalizeInputScale(value.inputScale);
		const auto strength = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 2.0f) : 1.0f; };
		value.transferStrength = strength(value.transferStrength);
		value.colourStrength = strength(value.colourStrength);
		value.maxRatio = std::isfinite(value.maxRatio) && value.maxRatio > 0 ? std::clamp(value.maxRatio, 0.01f, 16.0f) : 2.0f;
		value.whitePoint = std::isfinite(value.whitePoint) && value.whitePoint > 0 ? std::clamp(value.whitePoint, 0.0001f, 10000.0f) : 1.0f;
		return value;
	}
	inline ResolveMethod EffectiveResolve(const Reconstruction& value)
	{
		// Explicit Ratio always resolves; Auto/Residual at native
		// resolution both use the direct output, with no extra round trip.
		return value.method == ResolveMethod::Ratio ? ResolveMethod::Ratio :
			NormalizeInputScale(value.inputScale) < 1 ? ResolveMethod::Residual : ResolveMethod::Auto;
	}
	inline std::uint32_t WorkExtent(std::uint32_t extent, float scale)
	{
		return extent ? (std::max)(1u, static_cast<std::uint32_t>(std::llround(double(extent) * NormalizeInputScale(scale)))) : 0;
	}
	inline bool SameReconstructionResources(const Reconstruction& a, const Reconstruction& b)
	{
		return a.preset == b.preset && NormalizeInputScale(a.inputScale) == NormalizeInputScale(b.inputScale) &&
			EffectiveResolve(a) == EffectiveResolve(b) && a.colorIsHDR == b.colorIsHDR;
	}
	template<class Ini> Reconstruction LoadReconstruction(const Ini& ini, const char* section)
	{
		Reconstruction value;
		value.preset = static_cast<int>(ini.GetLongValue(section, "NRPreset", 0));
		value.method = static_cast<ResolveMethod>(ini.GetLongValue(section, "NRResolveMethod", 0));
		value.inputScale = static_cast<float>(ini.GetDoubleValue(section, "NRInputScale", 1));
		value.transferStrength = static_cast<float>(ini.GetDoubleValue(section, "NRTransferStrength", 1));
		value.colourStrength = static_cast<float>(ini.GetDoubleValue(section, "NRColourStrength", 1));
		value.maxRatio = static_cast<float>(ini.GetDoubleValue(section, "NRMaxRatio", 2));
		value.whitePoint = static_cast<float>(ini.GetDoubleValue(section, "NRWhitePoint", 1));
		value.colorIsHDR = ini.GetBoolValue(section, "NRColorIsHDR", false);
		return SanitizeReconstruction(value);
	}
	template<class Ini> void StoreReconstruction(Ini& ini, const char* section, Reconstruction value)
	{
		value = SanitizeReconstruction(value);
		ini.SetLongValue(section, "NRPreset", value.preset);
		ini.SetLongValue(section, "NRResolveMethod", static_cast<long>(value.method));
		ini.SetDoubleValue(section, "NRInputScale", value.inputScale);
		ini.SetDoubleValue(section, "NRTransferStrength", value.transferStrength);
		ini.SetDoubleValue(section, "NRColourStrength", value.colourStrength);
		ini.SetDoubleValue(section, "NRMaxRatio", value.maxRatio);
		ini.SetDoubleValue(section, "NRWhitePoint", value.whitePoint);
		ini.SetBoolValue(section, "NRColorIsHDR", value.colorIsHDR);
	}
}
