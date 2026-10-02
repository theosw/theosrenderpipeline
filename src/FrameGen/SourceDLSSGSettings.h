#pragma once

#include "NeuralRenderingPassSettings.h"
#include "NeuralRenderingReconstruction.h"
#include "NeuralCombatPolicy.h"
#include "SourceDLSSGGeneration.h"
#include "HDROutput.h"
#include "FrameGenerationProvider.h"
#include "XeFGOptions.h"

namespace TheosRenderPipeline::SourceDLSSG
{
	// Live runtime preferences. Presenter changes commit after Present; DLL paths
	// remain startup-owned and the NVIDIA test host still supplies DLSS and NR.
	struct Preferences
	{
		FrameGenerationProvider provider{FrameGenerationProvider::NVIDIA};
		XeFGOptions xefg{};
		int reflexMode{ 1 };
		int outputFPSLimit{ 0 };
		// Interpolate the HUD-less scene and UI layer separately. Live toggle.
		bool uiRecomposition{ true };
		// Give Intel XeFG the measured application frame time; on non-Intel GPUs
		// its software pacing uses this as a sanity check. Live toggle.
		bool xefgFrameTime{ true };
		GenerationRequest generation{};
		bool neuralEnabled{ false };
		bool neuralBeforeUpscaling{ true };
		int neuralPasses{ 1 };
		NeuralRendering::CombatSettings neuralCombat{};
		NeuralRendering::Tuning neuralTuning{};
		NeuralRendering::Reconstruction neuralReconstruction{};
		NeuralRendering::SecondPassSettings neuralSecondPass{};
		HDROutput::Settings hdrOutput{};
		bool operator==(const Preferences&) const = default;
	};

	inline Preferences SanitizePreferences(Preferences value)
	{
		if (!ValidProvider(static_cast<int>(value.provider))) { value.provider = FrameGenerationProvider::NVIDIA; }
		value.xefg = SanitizeXeFG(value.xefg);
		if (value.reflexMode < 0 || value.reflexMode > 2) { value.reflexMode = 1; }
		value.outputFPSLimit = std::clamp(value.outputFPSLimit, 0, 1000);
		value.generation = SanitizeGenerationRequest(value.generation);
		value.neuralTuning = NeuralRendering::SanitizeBuild14Tuning(value.neuralTuning);
		value.neuralPasses = std::clamp(value.neuralPasses, 1, 2);
		value.neuralCombat = NeuralRendering::SanitizeCombatSettings(value.neuralCombat);
		value.neuralReconstruction = NeuralRendering::SanitizeReconstruction(value.neuralReconstruction);
		value.neuralSecondPass = NeuralRendering::SanitizeSecondPass(value.neuralSecondPass);
		value.hdrOutput = HDROutput::Sanitize(value.hdrOutput);
		return value;
	}

	template <class Ini> Preferences LoadPreferences(const Ini& ini)
	{
		constexpr auto section = "SourceDLSSG";
		Preferences value;
		value.provider = static_cast<FrameGenerationProvider>(ini.GetLongValue("FrameGeneration", "Provider", 0));
		value.xefgFrameTime = ini.GetBoolValue("FrameGeneration", "XeFGFrameTime", true);
		value.xefg.experimentalMFG = ini.GetBoolValue("FrameGeneration", "XeFGExperimentalMFG", false);
		value.xefg.generatedFrames = static_cast<std::uint32_t>(std::clamp(ini.GetLongValue("FrameGeneration", "XeFGMultiplier", 2), 2L, static_cast<long>(XeFGMaxGeneratedFrames+1))-1);
		value.reflexMode = static_cast<int>(ini.GetLongValue(section, "ReflexMode", 1));
		// The original label said raster FPS, but the pinned runtime caps total
		// output. Preserve the old numeric value; never silently multiply it.
		value.outputFPSLimit = static_cast<int>(ini.GetLongValue(section, "OutputFPSLimit",
			ini.GetLongValue(section, "RasterFPSLimit", 0)));
		value.generation.generatedFrames = static_cast<std::uint32_t>(ini.GetLongValue(section, "GeneratedFrames", 1));
		value.generation.dynamic = ini.GetBoolValue(section, "DynamicMFG", false);
		value.generation.dynamicTargetFPS = static_cast<std::uint32_t>(ini.GetLongValue(section, "DynamicTargetFPS", 0));
		value.uiRecomposition = ini.GetBoolValue(section, "UIRecomposition", true);
		value.neuralEnabled = ini.GetBoolValue(section, "NeuralRenderingEnabled", false);
		value.neuralBeforeUpscaling = ini.GetBoolValue(section, "NRBeforeUpscaling", value.neuralBeforeUpscaling);
		value.neuralPasses = static_cast<int>(ini.GetLongValue(section, "NRPasses", 1));
		value.neuralCombat.inCombat = ini.GetBoolValue(section, "NROnePassInCombat", false);
		value.neuralCombat.weaponsDrawn = ini.GetBoolValue(section, "NROnePassWeaponsDrawn", false);
		value.neuralCombat.recoverySeconds = static_cast<float>(ini.GetDoubleValue(section, "NRPassRecoverySeconds", 5));
		auto& nr = value.neuralTuning;
		nr.style = static_cast<int>(ini.GetLongValue(section, "NRStyle", 0));
		nr.intensity = static_cast<float>(ini.GetDoubleValue(section, "NRIntensity", 1));
		nr.localToneStrength = static_cast<float>(ini.GetDoubleValue(section, "NRLocalTone", 1));
		nr.localStructureStrength = static_cast<float>(ini.GetDoubleValue(section, "NRLocalStructure", 1));
		nr.skinStructureStrength = static_cast<float>(ini.GetDoubleValue(section, "NRSkinStructure", 1));
		nr.useAutoSkinMask = ini.GetBoolValue(section, "NRAutoSkinMask", false);
		nr.uiCorrection = ini.GetBoolValue(section, "NRUICorrection", false);
		value.neuralReconstruction = NeuralRendering::LoadReconstruction(ini, section);
		value.neuralSecondPass = NeuralRendering::LoadSecondPass(ini, section, value.neuralReconstruction, value.neuralTuning);
		value.hdrOutput = HDROutput::Load(ini);
		return SanitizePreferences(value);
	}

	template <class Ini> void StorePreferences(Ini& ini, Preferences value)
	{
		value = SanitizePreferences(value);
		constexpr auto section = "SourceDLSSG";
		ini.SetLongValue("FrameGeneration", "Provider", static_cast<int>(value.provider));
		ini.SetBoolValue("FrameGeneration", "XeFGFrameTime", value.xefgFrameTime);
		ini.SetBoolValue("FrameGeneration", "XeFGExperimentalMFG", value.xefg.experimentalMFG);
		ini.SetLongValue("FrameGeneration", "XeFGMultiplier", value.xefg.generatedFrames+1);
		ini.SetLongValue(section, "ReflexMode", value.reflexMode);
		ini.SetLongValue(section, "OutputFPSLimit", value.outputFPSLimit);
		ini.Delete(section, "RasterFPSLimit");
		ini.SetLongValue(section, "GeneratedFrames", value.generation.generatedFrames);
		ini.SetBoolValue(section, "DynamicMFG", value.generation.dynamic);
		ini.SetLongValue(section, "DynamicTargetFPS", value.generation.dynamicTargetFPS);
		ini.SetBoolValue(section, "UIRecomposition", value.uiRecomposition);
		ini.SetBoolValue(section, "NeuralRenderingEnabled", value.neuralEnabled);
		ini.SetBoolValue(section, "NRBeforeUpscaling", value.neuralBeforeUpscaling);
		ini.SetLongValue(section, "NRPasses", value.neuralPasses);
		ini.SetBoolValue(section, "NROnePassInCombat", value.neuralCombat.inCombat);
		ini.SetBoolValue(section, "NROnePassWeaponsDrawn", value.neuralCombat.weaponsDrawn);
		ini.SetDoubleValue(section, "NRPassRecoverySeconds", value.neuralCombat.recoverySeconds);
		const auto& nr = value.neuralTuning;
		ini.SetLongValue(section, "NRStyle", nr.style);
		ini.SetDoubleValue(section, "NRIntensity", nr.intensity);
		ini.SetDoubleValue(section, "NRLocalTone", nr.localToneStrength);
		ini.SetDoubleValue(section, "NRLocalStructure", nr.localStructureStrength);
		ini.SetDoubleValue(section, "NRSkinStructure", nr.skinStructureStrength);
		ini.SetBoolValue(section, "NRAutoSkinMask", nr.useAutoSkinMask);
		ini.SetBoolValue(section, "NRUICorrection", nr.uiCorrection);
		NeuralRendering::StoreReconstruction(ini, section, value.neuralReconstruction);
		NeuralRendering::StoreSecondPass(ini, section, value.neuralSecondPass);
		HDROutput::Store(ini, value.hdrOutput);
	}
}
