#include "FrameGen/SourceDLSSGSettings.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
#include <SimpleIni.h>
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool value, const char* why)
{
	if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main()
{
	using namespace TheosRenderPipeline;
	using namespace SourceDLSSG;
	CSimpleIniA ini;
	auto empty = LoadPreferences(ini);
	Require(!empty.xefg.experimentalMFG && empty.xefg.generatedFrames == 1, "old INIs retain official XeFG x2");
	empty.xefg = {true, 5};
	empty.generation.generatedFrames = 5;
	StorePreferences(ini, empty);
	Require(LoadPreferences(ini) == empty, "XeFG x6 opt-in round trips without replacing NVIDIA x6");
	Require(!ini.GetValue("FrameGeneration", "XeFGExperimentalMFG"), "the separate opt-in key is retired");
	Require(SanitizePreferences(Preferences{.xefg = {false, 3}}).xefg.experimentalMFG &&
		!SanitizePreferences(Preferences{.xefg = {true, 1}}).xefg.experimentalMFG, "x3-x6 alone request the unlock");
	ini.SetBoolValue("FrameGeneration", "XeFGExperimentalMFG", false);
	Require(LoadPreferences(ini).xefg == XeFGOptions{false, 1}, "a development INI with the opt-in off stays x2");
	ini.SetBoolValue("FrameGeneration", "XeFGExperimentalMFG", true);
	Require(LoadPreferences(ini).xefg == XeFGOptions{true, 5}, "a development INI with the opt-in on keeps x6");
	ini.Delete("FrameGeneration", "XeFGExperimentalMFG");
	Require(XeFGCount({false, 3}, 3) == 1 && XeFGCount({true, 3}, 1) == 1, "disabled or refused MFG retains x2");
	Require(XeFGCount({true, 3}, 3) == 3 && XeFGCount({true, 2}, 3) == 2, "separate XeFG x3/x4 counts");
	Require(XeFGCount({true, 5}, 5) == 5 && XeFGCount({true, 5}, 3) == 3, "x6 respects admitted capacity");
	Require(XeFGCount({false, 5}, 5) == 1 && XeFGCount({true, 5}, 0) == 1,
		"resident unlock does not defeat disable or absent capacity");
	Require(SanitizeXeFG({true, 99}).generatedFrames == 5 && SanitizeXeFG({true, 0}).generatedFrames == 1,
		"bounded experimental counts");
	ini.SetLongValue("FrameGeneration", "XeFGMultiplier", 99);
	Require(LoadPreferences(ini).xefg.generatedFrames == 5, "excess INI multiplier clamps to x6");
	ini.SetLongValue("FrameGeneration", "XeFGMultiplier", 0);
	Require(LoadPreferences(ini).xefg.generatedFrames == 1, "invalid low INI multiplier clamps to x2");
	Require(XeFGOutputInterval(60) == 16667 && XeFGOutputInterval(0) == 0, "output cap does not scale with multiplier");
	ini.Reset();
	Require(ini.LoadData("[SourceDLSSG]\nNRPasses=2\nNRInputScale=0.75\nNRPreset=1\nNRIntensity=0.4\n") >= 0, "old INI");
	auto old = LoadPreferences(ini);
	Require(old.provider == FrameGenerationProvider::NVIDIA, "missing provider preserves NVIDIA startup");
	old.provider = FrameGenerationProvider::XeFG;
	old.generation.generatedFrames = 3;
	old.reflexMode = 2;
	StorePreferences(ini, old);
	Require(LoadPreferences(ini) == old, "XeFG selection retains NVIDIA multiplier/latency and all NR settings");
	ini.SetLongValue("FrameGeneration", "Provider", 42);
	Require(LoadPreferences(ini).provider == FrameGenerationProvider::NVIDIA, "unknown provider safely defaults to NVIDIA");
	StorePreferences(ini, old);
	Require(!old.neuralCombat.Enabled() && old.neuralCombat.recoverySeconds == 5, "old INI keeps combat policy off");
	old.neuralCombat = {true, true, 7.5f};
	Require(old.neuralSecondPass.linked && old.neuralSecondPass.inputScale == .75f && old.neuralSecondPass.preset == 1 &&
		old.neuralSecondPass.tuning.intensity == .4f, "old config inherits first pass and stays linked");
	old.neuralSecondPass.linked = false;
	old.neuralSecondPass.inputScale = .25f;
	old.neuralSecondPass.preset = 0;
	old.neuralSecondPass.tuning = { 7, .25f, 1.5f, .3f, -1, true, true };
	StorePreferences(ini, old);
	Require(LoadPreferences(ini) == old, "all pass 2 settings round trip independently");
	old.neuralSecondPass.linked = true;
	StorePreferences(ini, old);
	auto linked = LoadPreferences(ini);
	Require(linked == old, "relink preserves saved overrides");
	const auto effective = NeuralRendering::EffectiveSecondPass(linked.neuralSecondPass, linked.neuralReconstruction, linked.neuralTuning);
	Require(effective.inputScale == .75f && effective.preset == 1 && effective.tuning == linked.neuralTuning, "link overrides hidden custom values");
	NeuralOptions options; options.enabled = true; options.passes = 2; options.secondPass = old.neuralSecondPass;
	options.secondPass.linked = false; options.worldOnly = true;
	Require(!options.EffectiveSecond().tuning.uiCorrection && options.secondPass.tuning.uiCorrection, "world-only adapter suppresses UI correction without changing preference");
	NeuralHistory history;
	Require(history.ResetFor(options, true, false), "initial history");
	Require(!history.ResetFor(options, true, false), "stable history");
	options.secondPass.tuning.intensity = .8f;
	Require(history.ResetFor(options, true, false), "second tuning resets history");
	options.secondPass.linked = true;
	Require(history.ResetFor(options, true, false), "relink resets history");
	options.passOverride = NeuralRendering::PassOverride::Combat;
	Require(options.passes == 2 && options.EffectivePasses() == 1 && history.ResetFor(options, true, false),
		"override changes execution and resets image history without changing requested passes");
	options.passOverride = NeuralRendering::PassOverride::WeaponsDrawn;
	Require(!history.ResetFor(options, true, false), "reason-only changes preserve temporal history");
	options.passOverride = NeuralRendering::PassOverride::Recovery;
	Require(!history.ResetFor(options, true, false), "cooldown preserves temporal history");
	StorePreferences(ini, old);
	Require(LoadPreferences(ini) == old && LoadPreferences(ini).neuralPasses == 2,
		"saving while overridden preserves requested passes and independent tuning");
	options.passOverride = NeuralRendering::PassOverride::None;
	Require(options.EffectivePasses() == 2 && history.ResetFor(options, true, false), "restoration resets image history");
	options.combat = {true, false, 2};
	Require(!history.ResetFor(options, true, false), "policy settings alone do not reset image history");
	old.neuralSecondPass.inputScale = std::numeric_limits<float>::quiet_NaN();
	old.neuralSecondPass.preset = 99; old.neuralSecondPass.tuning.intensity = std::numeric_limits<float>::infinity();
	old = SanitizePreferences(old);
	Require(old.neuralSecondPass.inputScale == 1 && old.neuralSecondPass.preset == 0 && old.neuralSecondPass.tuning.intensity == 1, "invalid custom settings sanitized");
	std::puts("PASS: legacy defaults, independent INI persistence, relink preservation, sanitization and history/UI policy");
}
