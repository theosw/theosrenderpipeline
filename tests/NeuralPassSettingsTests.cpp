#include "FrameGen/SourceDLSSGSettings.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
#include <SimpleIni.h>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

static void Require(bool value, const char* why)
{
	if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main()
{
	using namespace TheosRenderPipeline;
	using namespace SourceDLSSG;
	CSimpleIniA ini;
    {
        CSimpleIniA saved;
        Preferences values;
        Require(!values.neuralObjectMasks.passes[0].Active() && !values.neuralObjectMasks.passes[1].Active(), "object rules default off");
        Require(!NeuralRendering::ObjectsInUse(values.neuralObjectMasks, 2), "no object capture by default");
        for(unsigned pass=0;pass<2;++pass)for(unsigned group=0;group<4;++group){
            values.neuralObjectMasks.passes[pass].groups[group]={group%2==0,float(pass+group)/5,float(group)/4,float(pass+group)/8};
        }
        StorePreferences(saved,values);
        Require(LoadPreferences(saved).neuralObjectMasks==values.neuralObjectMasks,"both passes' group controls round trip independently");
        CSimpleIniA old;
        old.SetBoolValue("SourceDLSSG", "NRObjectMasks", true);
        old.SetDoubleValue("SourceDLSSG", "NRFirstPersonStrength", 0);
        Require(!NeuralRendering::ObjectsInUse(LoadPreferences(old).neuralObjectMasks, 2),
            "released whole-chain object keys are ignored");
        old.SetBoolValue("SourceDLSSG", "NRObjectPass1Group1Enabled", true);
        const auto migrated=LoadPreferences(old).neuralObjectMasks;
        Require(migrated.passes[0].groups[0].enabled && migrated.passes[0].groups[0].strength==1,
            "a missing per-pass strength keeps the full pass");
        Require(NeuralRendering::ObjectsInUse(migrated, 1) && !NeuralRendering::ObjectsInUse(
            [&] { auto second = migrated; second.passes[0] = {}; second.passes[1].groups[2].enabled = true; return second; }(), 1),
            "capture follows rules in passes that run");
        StorePreferences(saved, LoadPreferences(old));
        Require(!saved.KeyExists("SourceDLSSG", "NRObjectMasks") && !saved.KeyExists("SourceDLSSG", "NRFirstPersonStrength"),
            "whole-chain object keys are not written");
        auto malformed=migrated;
        malformed.passes[0].groups[0].strength=std::numeric_limits<float>::quiet_NaN();
        malformed.passes[1].groups[0]={true,1,1,-3};
        malformed.passes[1].groups[1]={false,1,1,3};
        malformed=NeuralRendering::SanitizeObjectMask(malformed);
        Require(malformed.passes[0].groups[0].strength==1 && malformed.passes[1].Strengths()==std::array<float,4>{0,1,1,1},
            "invalid strengths sanitize and disabled groups retain full NR");
        values.neuralObjectMasks.previewGroups=0x5;
        values.neuralObjectMasks.previewClassesOnly=true;
        StorePreferences(saved,values);
        Require(!LoadPreferences(saved).neuralObjectMasks.previewGroups && !LoadPreferences(saved).neuralObjectMasks.previewClassesOnly,
            "diagnostic preview never persists");
        NeuralRendering::ObjectMaskSettings shown; shown.previewGroups=0x8;
        Require(NeuralRendering::ObjectsInUse(shown, 1), "showing a group in the preview turns capture on without rules");
        NeuralOptions options; options.enabled=true; options.automaticRuntime=true;
        options.objectMasks=values.neuralObjectMasks;
        NeuralHistory history; Require(history.ResetFor(options,true,false),"first object policy resets");
        Require(!history.ResetFor(options,true,false),"unchanged object policy retains history");
        options.objectMasks.previewGroups=0x6;
        options.objectMasks.previewClassesOnly=true;
        Require(!history.ResetFor(options,true,false),"preview edits do not invalidate inference history");
        options.objectMasks.passes[1].groups[1].tone=.125f;
        Require(history.ResetFor(options,true,false),"per-pass object controls invalidate history");
        options.objectMasks.passes[1].groups[1].strength=.875f;
        Require(history.ResetFor(options,true,false),"per-pass strength edits invalidate history");
        options.objectMasks.passes[0].groups[1].enabled=true; // starts disabled above
        Require(history.ResetFor(options,true,false),"enabling a pass object rule resets NR history");
    }

	auto empty = LoadPreferences(ini);
	Require(!empty.xefg.experimentalMFG && empty.xefg.generatedFrames == 1, "old INIs retain official XeFG x2");
	Require(empty.neuralRuntime == kAutomaticNeuralRuntime, "NR defaults to Automatic");
	{
		CSimpleIniA runtime;
		auto chosen = empty;
		chosen.neuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_int8.dll";
		StorePreferences(runtime, chosen);
		Require(std::string(runtime.GetValue("Experimental", "NeuralRenderingRuntimePath", "")) == chosen.neuralRuntime &&
			LoadPreferences(runtime).neuralRuntime == chosen.neuralRuntime, "the chosen runtime round trips through the existing key");
		chosen.neuralRuntime.clear();
		Require(SanitizePreferences(chosen).neuralRuntime == kAutomaticNeuralRuntime, "an empty choice returns to Automatic");
	}
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
	Require(XeFGNeedsRecreation(false, {true, 3}), "x3-x6 on a presenter created without the unlock recreates it");
	Require(!XeFGNeedsRecreation(true, {false, 1}) && !XeFGNeedsRecreation(true, {true, 3}),
		"a presenter that tried the unlock serves x2-x6 live, including a refused unlock at x2");
	Require(!XeFGNeedsRecreation(false, {false, 1}), "official x2 never recreates");
	Require(XeFGCount({false, 5}, 5) == 1 && XeFGCount({true, 5}, 0) == 1,
		"resident unlock does not defeat disable or absent capacity");
	Require(SanitizeXeFG({true, 99}).generatedFrames == 5 && SanitizeXeFG({true, 0}).generatedFrames == 1,
		"bounded experimental counts");
	ini.SetLongValue("FrameGeneration", "XeFGMultiplier", 99);
	Require(LoadPreferences(ini).xefg.generatedFrames == 5, "excess INI multiplier clamps to x6");
	ini.SetLongValue("FrameGeneration", "XeFGMultiplier", 0);
	Require(LoadPreferences(ini).xefg.generatedFrames == 1, "invalid low INI multiplier clamps to x2");
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
	Require(old.neuralExtraPasses[0].linked && old.neuralExtraPasses[0].inputScale == .75f && old.neuralExtraPasses[0].preset == 1 &&
		old.neuralExtraPasses[0].tuning.intensity == .4f, "old config inherits first pass and stays linked");
	old.neuralExtraPasses[0].linked = false;
	old.neuralExtraPasses[0].inputScale = .25f;
	old.neuralExtraPasses[0].preset = 0;
	old.neuralExtraPasses[0].tuning = { 7, .25f, 1.5f, .3f, -1, true, true };
	StorePreferences(ini, old);
	Require(LoadPreferences(ini) == old, "all pass 2 settings round trip independently");
	// Any custom style a runtime might accept survives; out-of-range values clamp.
	ini.SetLongValue("SourceDLSSG", "NRStyle", 200);
	ini.SetLongValue("SourceDLSSG", "NRPass2Style", -4);
	const auto custom = LoadPreferences(ini);
	Require(custom.neuralTuning.style == 200 && custom.neuralExtraPasses[0].tuning.style == 0, "custom styles load within 0 to 255");
	old.neuralExtraPasses[0].linked = true;
	StorePreferences(ini, old);
	auto linked = LoadPreferences(ini);
	Require(linked == old, "relink preserves saved overrides");
	const auto effective = NeuralRendering::EffectivePass(linked.neuralExtraPasses[0], linked.neuralReconstruction, linked.neuralTuning);
	Require(effective.inputScale == .75f && effective.preset == 1 && effective.tuning == linked.neuralTuning, "link overrides hidden custom values");
	NeuralOptions options; options.enabled = true; options.passes = 2; options.extraPasses[0] = old.neuralExtraPasses[0];
	options.extraPasses[0].linked = false; options.worldOnly = true;
	Require(!options.EffectiveExtra(0).tuning.uiCorrection && options.extraPasses[0].tuning.uiCorrection, "world-only adapter suppresses UI correction without changing preference");
	NeuralHistory history;
	Require(history.ResetFor(options, true, false), "initial history");
	Require(!history.ResetFor(options, true, false), "stable history");
	options.extraPasses[0].tuning.intensity = .8f;
	Require(history.ResetFor(options, true, false), "second tuning resets history");
	options.extraPasses[0].linked = true;
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
	old.neuralExtraPasses[0].inputScale = std::numeric_limits<float>::quiet_NaN();
	old.neuralExtraPasses[0].preset = 99; old.neuralExtraPasses[0].tuning.intensity = std::numeric_limits<float>::infinity();
	old = SanitizePreferences(old);
	Require(old.neuralExtraPasses[0].inputScale == 1 && old.neuralExtraPasses[0].preset == 0 && old.neuralExtraPasses[0].tuning.intensity == 1, "invalid custom settings sanitized");
	std::puts("PASS: legacy defaults, independent INI persistence, relink preservation, sanitization and history/UI policy");

	// Passes 3 to 5 follow Pass 2's pattern and keep Pass 2's released keys.
	{
		CSimpleIniA many;
		Require(many.LoadData("[SourceDLSSG]\nNRPasses=4\nNRInputScale=0.75\nNRPreset=1\nNRIntensity=0.4\n"
			"NRPass2UseSameSettings=false\nNRPass2InputScale=0.5\n") >= 0, "four-pass INI");
		auto loaded = LoadPreferences(many);
		Require(loaded.neuralPasses == 4, "four passes load");
		Require(!loaded.neuralExtraPasses[0].linked && loaded.neuralExtraPasses[0].inputScale == .5f, "Pass 2 keys keep their names");
		for (std::size_t i = 1; i < NeuralRendering::kExtraPasses; ++i) {
			const auto& extra = loaded.neuralExtraPasses[i];
			Require(extra.linked && extra.inputScale == .75f && extra.preset == 1 && extra.tuning.intensity == .4f,
				"a missing later pass inherits Pass 1 and stays linked");
		}
		for (long requested : {0L, 1L, 3L, 5L, 6L, 99L}) {
			many.SetLongValue("SourceDLSSG", "NRPasses", requested);
			Require(LoadPreferences(many).neuralPasses == std::clamp(static_cast<int>(requested), 1, NeuralRendering::kMaxNeuralPasses),
				"the pass count clamps to 1..5");
		}
		Require(SanitizePreferences(Preferences{ .neuralPasses = 7 }).neuralPasses == 5 &&
			SanitizeNeuralOptions(NeuralOptions{ .passes = 9 }).passes == 5, "preferences and options clamp to five passes");
		for (std::size_t i = 0; i < NeuralRendering::kExtraPasses; ++i) {
			auto& extra = loaded.neuralExtraPasses[i];
			extra.linked = i % 2 == 0;
			extra.inputScale = .25f + .125f * float(i);
			extra.preset = int(i % 2);
			extra.tuning = { int(3 + i), .2f + .1f * float(i), 1.25f, .5f, -1, i == 2, i != 1 };
		}
		loaded.neuralPasses = 5;
		StorePreferences(many, loaded);
		Require(LoadPreferences(many) == loaded, "every extra pass's settings round trip independently");
		for (const char* key : { "NRPass2UseSameSettings", "NRPass2InputScale", "NRPass3UseSameSettings", "NRPass4Style",
			"NRPass5UICorrection", "NRPass5LocalStructure", "NRObjectPass5Group4Enabled", "NRObjectPass3Group1Strength" }) {
			Require(many.KeyExists("SourceDLSSG", key), "extra-pass keys follow Pass 2's naming");
		}
		Require(!many.KeyExists("SourceDLSSG", "NRPass6InputScale") && !many.KeyExists("SourceDLSSG", "NRObjectPass6Group1Enabled"),
			"no keys past Pass 5");
		many.SetDoubleValue("SourceDLSSG", "NRPass4InputScale", std::numeric_limits<double>::quiet_NaN());
		many.SetLongValue("SourceDLSSG", "NRPass5Preset", 42);
		const auto sanitized = LoadPreferences(many);
		Require(sanitized.neuralExtraPasses[2].inputScale == 1 && sanitized.neuralExtraPasses[3].preset == 0 &&
			sanitized.neuralExtraPasses[1] == loaded.neuralExtraPasses[1], "a malformed later pass sanitizes alone");

		// Each extra pass follows Pass 1, never the pass before it.
		NeuralOptions chain; chain.enabled = true; chain.passes = 5;
		chain.reconstruction.inputScale = .8f; chain.reconstruction.preset = 1; chain.tuning.intensity = .9f;
		chain.extraPasses[0] = { false, .3f, 0, { 4, .2f } };
		chain.extraPasses[1].linked = true;
		chain.extraPasses[2] = { true, .4f, 0, { 6, .1f } }; // Stale values kept while linked.
		chain.extraPasses[3] = { false, .6f, 0, { 5, .7f } };
		for (std::size_t i = 1; i < 3; ++i) {
			const auto extra = chain.EffectiveExtra(i);
			Require(extra.linked && extra.inputScale == .8f && extra.preset == 1 && extra.tuning == chain.tuning,
				"a linked later pass takes Pass 1's settings even after an unlinked Pass 2");
		}
		Require(chain.EffectiveExtra(3).inputScale == .6f && chain.EffectiveExtra(3).tuning.intensity == .7f,
			"an unlinked Pass 5 keeps its own settings");
		// Reset to Pass 1 relinks one pass and keeps its own values for later.
		auto prefs = loaded;
		prefs.neuralExtraPasses[1].linked = false;
		prefs.neuralExtraPasses[2].linked = false;
		auto reset = prefs;
		reset.neuralExtraPasses[1].linked = true;
		StorePreferences(many, reset);
		const auto relinked = LoadPreferences(many);
		Require(relinked.neuralExtraPasses[1].linked && relinked.neuralExtraPasses[1].inputScale == prefs.neuralExtraPasses[1].inputScale &&
			relinked.neuralExtraPasses[1].tuning == prefs.neuralExtraPasses[1].tuning, "relinking Pass 3 keeps its own settings");
		for (std::size_t i : { 0u, 2u, 3u }) {
			Require(relinked.neuralExtraPasses[i] == prefs.neuralExtraPasses[i], "relinking one pass leaves the others");
		}

		// History follows every running pass, and pass reductions keep working with five.
		NeuralHistory passHistory;
		Require(passHistory.ResetFor(chain, true, false) && !passHistory.ResetFor(chain, true, false), "five-pass history");
		chain.extraPasses[3].tuning.localToneStrength = .3f;
		Require(passHistory.ResetFor(chain, true, false), "a Pass 5 tuning edit resets history");
		chain.passOverride = NeuralRendering::PassOverride::Combat;
		Require(chain.EffectivePasses() == 1 && passHistory.ResetFor(chain, true, false), "combat runs Pass 1 alone");
		chain.passOverride = NeuralRendering::PassOverride::None;
		chain.presetPasses = 3;
		Require(chain.EffectivePasses() == 3 && passHistory.ResetFor(chain, true, false), "a preset runs fewer passes");
		chain.passOverride = NeuralRendering::PassOverride::Preset;
		Require(chain.EffectivePasses() == 3 && !passHistory.ResetFor(chain, true, false), "the preset label alone keeps history");
		chain.passOverride = NeuralRendering::PassOverride::Combat;
		Require(chain.EffectivePasses() == 1, "combat still runs one pass inside a preset's three");
		chain.presetPasses = 7; chain.passOverride = NeuralRendering::PassOverride::None;
		Require(chain.EffectivePasses() == 5, "a preset never adds passes past the allocation");

		// Object rules exist per pass for all five.
		NeuralRendering::ObjectMaskSettings masks;
		masks.passes[3].groups[1] = { true, .5f, .25f, .75f };
		masks.passes[4].groups[3] = { true, 0, 1, .5f };
		Require(!NeuralRendering::ObjectsInUse(masks, 3) && NeuralRendering::ObjectsInUse(masks, 4) &&
			NeuralRendering::ObjectsInUse(masks, 9), "capture follows the rules of passes 4 and 5 only when they run");
		auto withMasks = loaded; withMasks.neuralObjectMasks = masks;
		StorePreferences(many, withMasks);
		Require(LoadPreferences(many).neuralObjectMasks == masks, "Pass 4 and 5 object rules round trip");
	}

	// Pass 1 and 2 start together as before; each later feature is created
	// alone and runs from the next frame.
	{
		using NeuralRendering::FeatureRamp;
		using NeuralRendering::NextFeatureRamp;
		Require(NextFeatureRamp(0, 1) == FeatureRamp{ 1, 1 } && NextFeatureRamp(1, 1) == FeatureRamp{ 1, 1 }, "one pass");
		Require(NextFeatureRamp(0, 2) == FeatureRamp{ 2, 2 } && NextFeatureRamp(2, 2) == FeatureRamp{ 2, 2 }, "two passes start together");
		for (int passes = 3; passes <= NeuralRendering::kMaxNeuralPasses; ++passes) {
			int created = 0, frames = 0;
			std::vector<int> ready;
			while (true) {
				const auto step = NextFeatureRamp(created, passes);
				Require(step.created - created <= (created ? 1 : 2), "at most one new feature per frame after the first");
				Require(step.ready <= (std::max)(created, step.created) && (created == 0 || step.ready == created),
					"a later feature never runs on its creation frame");
				ready.push_back(step.ready);
				created = step.created;
				if (++frames > 10 || step.ready == passes) { break; }
			}
			// For five: 2, 2, 3, 4, 5 passes over five frames.
			Require(created == passes && ready.front() == 2 && ready[1] == 2 && ready.back() == passes &&
				frames == passes, "every pass becomes ready, one more each frame after the second");
		}
		// A refused later pass caps the chain at the passes before it; they keep running.
		Require(NextFeatureRamp(1, 2, 1) == FeatureRamp{ 1, 1 }, "refused Pass 2 leaves Pass 1 running");
		Require(NextFeatureRamp(3, 5, 3) == FeatureRamp{ 3, 3 } && NextFeatureRamp(2, 5, 3) == FeatureRamp{ 3, 2 },
			"refused Pass 4 stops the ramp at three passes");
		Require(NextFeatureRamp(0, 5, 0) == FeatureRamp{ 1, 1 }, "Pass 1 is always attempted");
	}
	std::puts("PASS: Pass 3-5 keys, 1..5 clamp, Pass 1 linking per pass, single-pass reset, five-pass history/overrides, "
		"object rules per pass, the one-new-feature-per-frame ramp and its refused-pass cap");
}
