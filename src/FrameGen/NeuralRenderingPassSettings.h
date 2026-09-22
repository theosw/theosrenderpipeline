#pragma once
#include "NeuralRenderingReconstruction.h"
#include "NeuralRenderingTuning.h"

namespace TheosRenderPipeline::NeuralRendering
{
	struct SecondPassSettings
	{
		bool linked{ true };
		float inputScale{ 1 };
		int preset{};
		Tuning tuning{};
		bool operator==(const SecondPassSettings&) const = default;
	};
	inline SecondPassSettings SanitizeSecondPass(SecondPassSettings value)
	{
		value.inputScale = NormalizeInputScale(value.inputScale);
		if (value.preset < 0 || value.preset > 1) { value.preset = 0; }
		value.tuning = SanitizeBuild14Tuning(value.tuning);
		return value;
	}
	inline SecondPassSettings EffectiveSecondPass(const SecondPassSettings& value, const Reconstruction& first, const Tuning& tuning)
	{
		return SanitizeSecondPass(value.linked ? SecondPassSettings{ true, first.inputScale, first.preset, tuning } : value);
	}
	template<class Ini> SecondPassSettings LoadSecondPass(const Ini& ini, const char* section, const Reconstruction& first, const Tuning& tuning)
	{
		// Missing overrides inherit the old shared settings. Relinking never erases
		// saved overrides, so experimenting with the checkbox is reversible.
		SecondPassSettings value;
		value.linked = ini.GetBoolValue(section, "NRPass2UseSameSettings", true);
		value.inputScale = static_cast<float>(ini.GetDoubleValue(section, "NRPass2InputScale", first.inputScale));
		value.preset = static_cast<int>(ini.GetLongValue(section, "NRPass2Preset", first.preset));
		auto& t = value.tuning;
		t.style = static_cast<int>(ini.GetLongValue(section, "NRPass2Style", tuning.style));
		t.intensity = static_cast<float>(ini.GetDoubleValue(section, "NRPass2Intensity", tuning.intensity));
		t.localToneStrength = static_cast<float>(ini.GetDoubleValue(section, "NRPass2LocalTone", tuning.localToneStrength));
		t.localStructureStrength = static_cast<float>(ini.GetDoubleValue(section, "NRPass2LocalStructure", tuning.localStructureStrength));
		t.skinStructureStrength = static_cast<float>(ini.GetDoubleValue(section, "NRPass2SkinStructure", tuning.skinStructureStrength));
		t.useAutoSkinMask = ini.GetBoolValue(section, "NRPass2AutoSkinMask", tuning.useAutoSkinMask);
		t.uiCorrection = ini.GetBoolValue(section, "NRPass2UICorrection", tuning.uiCorrection);
		return SanitizeSecondPass(value);
	}
	template<class Ini> void StoreSecondPass(Ini& ini, const char* section, SecondPassSettings value)
	{
		value = SanitizeSecondPass(value);
		ini.SetBoolValue(section, "NRPass2UseSameSettings", value.linked);
		ini.SetDoubleValue(section, "NRPass2InputScale", value.inputScale);
		ini.SetLongValue(section, "NRPass2Preset", value.preset);
		const auto& t = value.tuning;
		ini.SetLongValue(section, "NRPass2Style", t.style);
		ini.SetDoubleValue(section, "NRPass2Intensity", t.intensity);
		ini.SetDoubleValue(section, "NRPass2LocalTone", t.localToneStrength);
		ini.SetDoubleValue(section, "NRPass2LocalStructure", t.localStructureStrength);
		ini.SetDoubleValue(section, "NRPass2SkinStructure", t.skinStructureStrength);
		ini.SetBoolValue(section, "NRPass2AutoSkinMask", t.useAutoSkinMask);
		ini.SetBoolValue(section, "NRPass2UICorrection", t.uiCorrection);
	}
}
