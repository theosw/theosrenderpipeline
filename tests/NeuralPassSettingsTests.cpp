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
	Require(ini.LoadData("[SourceDLSSG]\nNRPasses=2\nNRInputScale=0.75\nNRPreset=1\nNRIntensity=0.4\n") >= 0, "old INI");
	auto old = LoadPreferences(ini);
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
	old.neuralSecondPass.inputScale = std::numeric_limits<float>::quiet_NaN();
	old.neuralSecondPass.preset = 99; old.neuralSecondPass.tuning.intensity = std::numeric_limits<float>::infinity();
	old = SanitizePreferences(old);
	Require(old.neuralSecondPass.inputScale == 1 && old.neuralSecondPass.preset == 0 && old.neuralSecondPass.tuning.intensity == 1, "invalid custom settings sanitized");
	std::puts("PASS: legacy defaults, independent INI persistence, relink preservation, sanitization and history/UI policy");
}
