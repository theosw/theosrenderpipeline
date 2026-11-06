#pragma once

#include <string>
#include <string_view>

#include "NeuralRenderingPassSettings.h"
#include "NeuralRenderingReconstruction.h"
#include "NeuralCombatPolicy.h"
#include "NeuralObjectMask.h"
#include "SourceDLSSGGeneration.h"
#include "HDROutput.h"
#include "FrameGenerationProvider.h"
#include "XeFGOptions.h"
#include "PresentationVSync.h"
#include "DlssgPreset.h"
#include "SteadyPacing.h"
#include "NeuralRuntimeFiles.h"

namespace TheosRenderPipeline::SourceDLSSG
{
	// Live runtime preferences. Presenter changes commit after Present. The NR
	// runtime file is chosen here; the Streamline directory remains startup-owned.
	struct Preferences
	{
		FrameGenerationProvider provider{FrameGenerationProvider::NVIDIA};
		VSyncMode vsync{VSyncMode::Auto};
		XeFGOptions xefg{};
		int reflexMode{ 1 };
		int outputFPSLimit{ 0 };
		FrameLimitMode frameLimit{ FrameLimitMode::Off };
		// NVIDIA's Frame Generation preset: B recomposes the UI (also XeFG's
		// UI layer), C uses DLSS-G 310.9.16+ Lite blending. Live; recreates DLSS-G.
		DlssgPreset preset{ DlssgPreset::B };
		// Give Intel XeFG the measured application frame time; on non-Intel GPUs
		// its software pacing uses this as a sanity check. Live toggle.
		bool xefgFrameTime{ true };
		// Optional private Universal backend; runtime selection needs a restart.
		bool xefgTensorCores{ false };
		// XeFG x3/x4 with VSync: space generated frames evenly in software when
		// NVIDIA flip metering is unavailable. Costs base FPS; recreates XeFG.
		bool xefgEvenPacing{ false };
		// Intel XeFG scene-change sensitivity, 0-1. Live.
		float xefgSceneChangeThreshold{ XeFGDefaultSceneChangeThreshold };
		GenerationRequest generation{};
		bool neuralEnabled{ false };
		// Automatic, or an NR runtime DLL relative to Data/SKSE/Plugins unless
		// absolute. Any file loads by its version; see NeuralRuntimeFiles.h.
		std::string neuralRuntime{ kAutomaticNeuralRuntime };
		bool neuralBeforeUpscaling{ true };
		int neuralPasses{ 1 };
		NeuralRendering::CombatSettings neuralCombat{};
		NeuralRendering::Tuning neuralTuning{};
        NeuralRendering::ObjectMaskSettings neuralObjectMasks{};
		NeuralRendering::Reconstruction neuralReconstruction{};
		// Passes 2 to 5, used up to neuralPasses.
		NeuralRendering::ExtraPasses neuralExtraPasses{};
		HDROutput::Settings hdrOutput{};
		bool operator==(const Preferences&) const = default;
	};

	inline Preferences SanitizePreferences(Preferences value)
	{
		if (!ValidProvider(static_cast<int>(value.provider))) { value.provider = FrameGenerationProvider::NVIDIA; }
		value.vsync = SanitizeVSync(value.vsync);
		value.xefg = SanitizeXeFG(value.xefg);
		value.xefgSceneChangeThreshold = SanitizeXeFGSceneChange(value.xefgSceneChangeThreshold);
		// The multiplier is the only user choice: x3-x6 request the experimental unlock.
		value.xefg.experimentalMFG = value.xefg.generatedFrames > 1;
		if (value.reflexMode < 0 || value.reflexMode > 2) { value.reflexMode = 1; }
		value.outputFPSLimit = std::clamp(value.outputFPSLimit, 0, 1000);
		if (static_cast<int>(value.frameLimit) < 0 || static_cast<int>(value.frameLimit) > 2) { value.frameLimit = FrameLimitMode::Off; }
		if (value.frameLimit == FrameLimitMode::Fixed && !value.outputFPSLimit) { value.frameLimit = FrameLimitMode::Off; }
		value.generation = SanitizeGenerationRequest(value.generation);
		if (!ValidPreset(static_cast<int>(value.preset))) { value.preset = DlssgPreset::B; }
		value.neuralTuning = NeuralRendering::SanitizeBuild14Tuning(value.neuralTuning);
        value.neuralObjectMasks = NeuralRendering::SanitizeObjectMask(value.neuralObjectMasks);
		if (value.neuralRuntime.empty()) { value.neuralRuntime = kAutomaticNeuralRuntime; }
		value.neuralPasses = std::clamp(value.neuralPasses, 1, NeuralRendering::kMaxNeuralPasses);
		value.neuralCombat = NeuralRendering::SanitizeCombatSettings(value.neuralCombat);
		value.neuralReconstruction = NeuralRendering::SanitizeReconstruction(value.neuralReconstruction);
		value.neuralExtraPasses = NeuralRendering::SanitizeExtraPasses(value.neuralExtraPasses);
		value.hdrOutput = HDROutput::Sanitize(value.hdrOutput);
		return value;
	}

	template <class Ini> Preferences LoadPreferences(const Ini& ini)
	{
		constexpr auto section = "SourceDLSSG";
		Preferences value;
		value.provider = static_cast<FrameGenerationProvider>(ini.GetLongValue("FrameGeneration", "Provider", 0));
		const auto vsync = ini.GetLongValue("FrameGeneration", "VSync", 0);
		value.vsync = vsync >= 0 && vsync <= 2 ? static_cast<VSyncMode>(vsync) : VSyncMode::Auto;
		value.xefgFrameTime = ini.GetBoolValue("FrameGeneration", "XeFGFrameTime", true);
		value.xefgTensorCores = ini.GetBoolValue("FrameGeneration", "XeFGTensorCores", false);
		value.xefgEvenPacing = ini.GetBoolValue("FrameGeneration", "XeFGEvenPacing", false);
		value.xefgSceneChangeThreshold = static_cast<float>(ini.GetDoubleValue("FrameGeneration", "XeFGSceneChangeThreshold",
			XeFGDefaultSceneChangeThreshold));
		value.xefg.generatedFrames = static_cast<std::uint32_t>(std::clamp(ini.GetLongValue("FrameGeneration", "XeFGMultiplier", 2), 2L, static_cast<long>(XeFGMaxGeneratedFrames+1))-1);
		// Development INIs kept a multiplier behind a separate opt-in; off meant x2.
		if (!ini.GetBoolValue("FrameGeneration", "XeFGExperimentalMFG", true)) { value.xefg.generatedFrames = 1; }
		value.reflexMode = static_cast<int>(ini.GetLongValue(section, "ReflexMode", 1));
		// The original label said raster FPS, but the pinned runtime caps total
		// output. Preserve the old numeric value; never silently multiply it.
		value.outputFPSLimit = static_cast<int>(ini.GetLongValue(section, "OutputFPSLimit",
			ini.GetLongValue(section, "RasterFPSLimit", 0)));
		// Without the key, a saved output cap means Fixed.
		value.frameLimit = static_cast<FrameLimitMode>(ini.GetLongValue(section, "FrameLimit",
			static_cast<long>(value.outputFPSLimit > 0 ? FrameLimitMode::Fixed : FrameLimitMode::Off)));
		value.generation.generatedFrames = static_cast<std::uint32_t>(ini.GetLongValue(section, "GeneratedFrames", 1));
		value.generation.dynamic = ini.GetBoolValue(section, "DynamicMFG", false);
		value.generation.dynamicTargetFPS = static_cast<std::uint32_t>(ini.GetLongValue(section, "DynamicTargetFPS", 0));
		// Earlier INIs saved only UIRecomposition: on is Preset B, off is A.
		value.preset = static_cast<DlssgPreset>(ini.GetLongValue(section, "FrameGenerationPreset",
			ini.GetBoolValue(section, "UIRecomposition", true) ? 2 : 1));
		value.neuralEnabled = ini.GetBoolValue(section, "NeuralRenderingEnabled", false);
		value.neuralRuntime = NormalizeNeuralRuntimeSetting(
			ini.GetValue("Experimental", "NeuralRenderingRuntimePath", std::string(kAutomaticNeuralRuntime).c_str()),
			ini.GetBoolValue("Experimental", "NeuralRenderingRuntimeExplicit", false));
		value.neuralBeforeUpscaling = ini.GetBoolValue(section, "NRBeforeUpscaling", value.neuralBeforeUpscaling);
		value.neuralPasses = static_cast<int>(ini.GetLongValue(section, "NRPasses", 1));
		value.neuralCombat.inCombat = ini.GetBoolValue(section, "NROnePassInCombat", false);
		value.neuralCombat.weaponsDrawn = ini.GetBoolValue(section, "NROnePassWeaponsDrawn", false);
		value.neuralCombat.recoverySeconds = static_cast<float>(ini.GetDoubleValue(section, "NRPassRecoverySeconds", 5));
		auto& nr = value.neuralTuning;
		nr.style = std::clamp(static_cast<int>(ini.GetLongValue(section, "NRStyle", 0)), 0, 255);
		nr.intensity = static_cast<float>(ini.GetDoubleValue(section, "NRIntensity", 1));
		nr.localToneStrength = static_cast<float>(ini.GetDoubleValue(section, "NRLocalTone", 1));
		nr.localStructureStrength = static_cast<float>(ini.GetDoubleValue(section, "NRLocalStructure", 1));
		nr.skinStructureStrength = static_cast<float>(ini.GetDoubleValue(section, "NRSkinStructure", 1));
		nr.useAutoSkinMask = ini.GetBoolValue(section, "NRAutoSkinMask", false);
		nr.uiCorrection = ini.GetBoolValue(section, "NRUICorrection", false);
		value.neuralReconstruction = NeuralRendering::LoadReconstruction(ini, section);
		value.neuralExtraPasses = NeuralRendering::LoadExtraPasses(ini, section, value.neuralReconstruction, value.neuralTuning);
        value.neuralObjectMasks = NeuralRendering::LoadObjectMask(ini, section);
		value.hdrOutput = HDROutput::Load(ini);
		return SanitizePreferences(value);
	}

	template <class Ini> void StorePreferences(Ini& ini, Preferences value)
	{
		value = SanitizePreferences(value);
		constexpr auto section = "SourceDLSSG";
		ini.SetLongValue("FrameGeneration", "Provider", static_cast<int>(value.provider));
		ini.SetLongValue("FrameGeneration", "VSync", static_cast<int>(value.vsync));
		ini.SetBoolValue("FrameGeneration", "XeFGFrameTime", value.xefgFrameTime);
		ini.SetBoolValue("FrameGeneration", "XeFGTensorCores", value.xefgTensorCores);
		ini.SetBoolValue("FrameGeneration", "XeFGEvenPacing", value.xefgEvenPacing);
		ini.SetDoubleValue("FrameGeneration", "XeFGSceneChangeThreshold", value.xefgSceneChangeThreshold);
		ini.Delete("FrameGeneration", "XeFGExperimentalMFG");
		ini.SetLongValue("FrameGeneration", "XeFGMultiplier", value.xefg.generatedFrames+1);
		ini.SetLongValue(section, "ReflexMode", value.reflexMode);
		ini.SetLongValue(section, "OutputFPSLimit", value.outputFPSLimit);
		ini.SetLongValue(section, "FrameLimit", static_cast<long>(value.frameLimit));
		ini.Delete(section, "RasterFPSLimit");
		ini.SetLongValue(section, "GeneratedFrames", value.generation.generatedFrames);
		ini.SetBoolValue(section, "DynamicMFG", value.generation.dynamic);
		ini.SetLongValue(section, "DynamicTargetFPS", value.generation.dynamicTargetFPS);
		ini.SetLongValue(section, "FrameGenerationPreset", static_cast<long>(value.preset));
		// Kept so an older TRP reading this INI still gets the UI choice.
		ini.SetBoolValue(section, "UIRecomposition", UsesUIRecomposition(value.preset));
		ini.SetBoolValue(section, "NeuralRenderingEnabled", value.neuralEnabled);
		ini.SetValue("Experimental", "NeuralRenderingRuntimePath", value.neuralRuntime.c_str());
		// Keeps a chosen 310.8 from being read back as Automatic.
		ini.SetBoolValue("Experimental", "NeuralRenderingRuntimeExplicit", !IsAutomaticNeuralRuntime(value.neuralRuntime));
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
		NeuralRendering::StoreExtraPasses(ini, section, value.neuralExtraPasses);
        NeuralRendering::StoreObjectMask(ini, section, value.neuralObjectMasks);
		HDROutput::Store(ini, value.hdrOutput);
	}
}
