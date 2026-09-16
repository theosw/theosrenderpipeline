#pragma once

#include "NeuralRenderingTuning.h"
#include "NeuralRenderingReconstruction.h"
#include "SourceDLSSGGeneration.h"

namespace TheosRenderPipeline::SourceDLSSG
{
	// Runtime preferences only. Backend selection and DLL paths stay startup-owned.
	struct Preferences
	{
		int reflexMode{ 1 };
		int outputFPSLimit{ 0 };
		GenerationRequest generation{};
		bool neuralEnabled{ false };
		bool neuralBeforeUpscaling{ true };
		int neuralPasses{ 1 };
		NeuralRendering::Tuning neuralTuning{};
		NeuralRendering::Reconstruction neuralReconstruction{};
		bool operator==(const Preferences&) const = default;
	};

	inline Preferences SanitizePreferences(Preferences value)
	{
		if (value.reflexMode < 0 || value.reflexMode > 2) { value.reflexMode = 1; }
		value.outputFPSLimit = std::clamp(value.outputFPSLimit, 0, 1000);
		if (!ValidGenerationRequest(value.generation)) { value.generation = {}; }
		value.neuralTuning = NeuralRendering::SanitizeBuild14Tuning(value.neuralTuning);
		value.neuralPasses = std::clamp(value.neuralPasses, 1, 2);
		value.neuralReconstruction = NeuralRendering::SanitizeReconstruction(value.neuralReconstruction);
		return value;
	}

	template <class Ini> Preferences LoadPreferences(const Ini& ini)
	{
		constexpr auto section = "SourceDLSSG";
		Preferences value;
		value.reflexMode = static_cast<int>(ini.GetLongValue(section, "ReflexMode", 1));
		// The original label said raster FPS, but the pinned runtime caps total
		// output. Preserve the old numeric value; never silently multiply it.
		value.outputFPSLimit = static_cast<int>(ini.GetLongValue(section, "OutputFPSLimit",
			ini.GetLongValue(section, "RasterFPSLimit", 0)));
		value.generation.generatedFrames = static_cast<std::uint32_t>(ini.GetLongValue(section, "GeneratedFrames", 1));
		value.generation.dynamic = ini.GetBoolValue(section, "DynamicMFG", false);
		value.generation.dynamicTargetFPS = static_cast<std::uint32_t>(ini.GetLongValue(section, "DynamicTargetFPS", 0));
		value.neuralEnabled = ini.GetBoolValue(section, "NeuralRenderingEnabled", false);
		value.neuralBeforeUpscaling = ini.GetBoolValue(section, "NRBeforeUpscaling", value.neuralBeforeUpscaling);
		value.neuralPasses = static_cast<int>(ini.GetLongValue(section, "NRPasses", 1));
		auto& nr = value.neuralTuning;
		nr.style = static_cast<int>(ini.GetLongValue(section, "NRStyle", 0));
		nr.intensity = static_cast<float>(ini.GetDoubleValue(section, "NRIntensity", 1));
		nr.localToneStrength = static_cast<float>(ini.GetDoubleValue(section, "NRLocalTone", 1));
		nr.localStructureStrength = static_cast<float>(ini.GetDoubleValue(section, "NRLocalStructure", 1));
		nr.skinStructureStrength = static_cast<float>(ini.GetDoubleValue(section, "NRSkinStructure", 1));
		nr.useAutoSkinMask = ini.GetBoolValue(section, "NRAutoSkinMask", false);
		nr.uiCorrection = ini.GetBoolValue(section, "NRUICorrection", false);
		value.neuralReconstruction = NeuralRendering::LoadReconstruction(ini, section);
		return SanitizePreferences(value);
	}

	template <class Ini> void StorePreferences(Ini& ini, Preferences value)
	{
		value = SanitizePreferences(value);
		constexpr auto section = "SourceDLSSG";
		ini.SetLongValue(section, "ReflexMode", value.reflexMode);
		ini.SetLongValue(section, "OutputFPSLimit", value.outputFPSLimit);
		ini.Delete(section, "RasterFPSLimit");
		ini.SetLongValue(section, "GeneratedFrames", value.generation.generatedFrames);
		ini.SetBoolValue(section, "DynamicMFG", value.generation.dynamic);
		ini.SetLongValue(section, "DynamicTargetFPS", value.generation.dynamicTargetFPS);
		ini.SetBoolValue(section, "NeuralRenderingEnabled", value.neuralEnabled);
		ini.SetBoolValue(section, "NRBeforeUpscaling", value.neuralBeforeUpscaling);
		ini.SetLongValue(section, "NRPasses", value.neuralPasses);
		const auto& nr = value.neuralTuning;
		ini.SetLongValue(section, "NRStyle", nr.style);
		ini.SetDoubleValue(section, "NRIntensity", nr.intensity);
		ini.SetDoubleValue(section, "NRLocalTone", nr.localToneStrength);
		ini.SetDoubleValue(section, "NRLocalStructure", nr.localStructureStrength);
		ini.SetDoubleValue(section, "NRSkinStructure", nr.skinStructureStrength);
		ini.SetBoolValue(section, "NRAutoSkinMask", nr.useAutoSkinMask);
		ini.SetBoolValue(section, "NRUICorrection", nr.uiCorrection);
		NeuralRendering::StoreReconstruction(ini, section, value.neuralReconstruction);
	}
}
