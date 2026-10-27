#include "FrameGen/NeuralModelRate.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace TheosRenderPipeline::NeuralRendering;
using TheosRenderPipeline::SourceDLSSG::NeuralOptions;
using TheosRenderPipeline::SourceDLSSG::SameModelOutputOptions;
using TheosRenderPipeline::SourceDLSSG::SameHistoryOptions;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static ModelRateFrame Alternate(bool reset = false, bool changed = false, bool frozen = false)
{
    return { ModelRate::EveryOtherFrame, reset, changed, frozen };
}

int main()
{
    {
        TheosRenderPipeline::SourceDLSSG::NeuralTelemetryTracker timing;
        Require(!timing.Snapshot().frameSamples, "absent timing is not a measured zero");
        timing.RecordFrame(0, 9000, 1'000'000);
        timing.RecordFrame(0, 2000, 1'000'000);
        timing.RecordFrame(0, 1000, 1'000'000);
        Require(timing.Snapshot().AverageFrameGPUMicroseconds() == 4000, "measured per-frame work includes both skipped frames");
        timing.RecordFrame(2, 1, 1'000'000); timing.RecordFrame(0, 1, 0); timing.RecordFrame(0, 1, 2'000'000);
        Require(timing.Snapshot().frameSamples == 3 && timing.Snapshot().frameQueryFailures == 3,
            "invalid or cross-domain timestamps cannot enter the average");
        for (unsigned i = 0; i < 128; ++i) { timing.RecordFrame(0, 1000, 1'000'000); }
        Require(timing.Snapshot().frameSamples == 128 && timing.Snapshot().AverageFrameGPUMicroseconds() == 1000,
            "recent window expires older settings and rate costs");
    }
    // Every frame evaluates and keeps nothing.
    {
        ModelRateSchedule schedule;
        for (int frame = 0; frame < 6; ++frame) {
            const auto d = schedule.Next({});
            Require(d.evaluate && !d.composeMotion && !d.captureGuides && !d.storeChange, "every frame runs the model");
        }
    }
    // Every other frame alternates from the first evaluation, never skips twice,
    // and composes motion on each evaluation that follows a skip.
    {
        ModelRateSchedule schedule;
        std::vector<bool> evaluated;
        for (int frame = 0; frame < 8; ++frame) {
            const auto d = schedule.Next(Alternate(frame == 0));
            evaluated.push_back(d.evaluate);
            Require(d.captureGuides, "guides are kept on every alternating frame");
            Require(d.storeChange == d.evaluate, "every evaluation stores its change");
            Require(d.composeMotion == (d.evaluate && frame > 0), "an evaluation after a skip composes two frames of motion");
        }
        Require(evaluated == std::vector<bool>{ true, false, true, false, true, false, true, false }, "alternates");
    }
    // Every third frame: two skips per evaluation. The first skip stores the change
    // it carried, the second accumulates motion back to the evaluation, and the
    // evaluation after them composes it. Every other frame keeps its decisions.
    {
        ModelRateSchedule schedule;
        std::vector<bool> evaluated;
        for (int frame = 0; frame < 9; ++frame) {
            const auto d = schedule.Next({ ModelRate::EveryThirdFrame, frame == 0 });
            evaluated.push_back(d.evaluate);
            const int phase = frame % 3;  // 0 evaluates, 1 and 2 skip
            Require(d.captureGuides, "guides are kept on every frame of the cycle");
            Require(d.storeChange == (phase != 2), "the evaluation and the first skip store their change");
            Require(d.accumulateMotion == (phase == 2), "the second skip accumulates motion to the evaluation");
            Require(d.composeMotion == (phase == 0 && frame > 0), "an evaluation after two skips composes their motion");
        }
        Require(evaluated == std::vector<bool>{ true, false, false, true, false, false, true, false, false }, "one in three");
        Require(ModelRateInterval(ModelRate::EveryThirdFrame) == 3 && ModelRateInterval(ModelRate::EveryOtherFrame) == 2 &&
            ModelRateInterval(ModelRate::EveryFrame) == 1 && ModelRateInterval(static_cast<ModelRate>(9)) == 1, "intervals");
    }
    // A change on the second frame of the cycle evaluates and starts the cycle again.
    {
        ModelRateSchedule schedule;
        Require(schedule.Next({ ModelRate::EveryThirdFrame }).evaluate, "evaluates first");
        Require(!schedule.Next({ ModelRate::EveryThirdFrame }).evaluate, "first skip");
        auto d = schedule.Next({ ModelRate::EveryThirdFrame, false, true });
        Require(d.evaluate && d.composeMotion && !d.accumulateMotion, "a change evaluates and bridges the one skip");
        Require(!schedule.Next({ ModelRate::EveryThirdFrame }).evaluate && !schedule.Next({ ModelRate::EveryThirdFrame }).evaluate,
            "then skips twice");
        d = schedule.Next({ ModelRate::EveryOtherFrame });
        Require(d.evaluate && d.composeMotion, "switching to every other frame after two skips evaluates first");
        Require(!schedule.Next({ ModelRate::EveryOtherFrame }).evaluate, "and alternates");
    }
    // A reset or change forces an evaluation; a reset after a skip has no history to bridge.
    {
        ModelRateSchedule schedule;
        Require(schedule.Next(Alternate()).evaluate, "the first frame has nothing stored");
        Require(!schedule.Next(Alternate()).evaluate, "second frame skips");
        auto d = schedule.Next(Alternate(true));
        Require(d.evaluate && !d.composeMotion, "reset evaluates without composition");
        d = schedule.Next(Alternate(false, true));
        Require(d.evaluate && !d.composeMotion, "a change evaluates; the previous frame evaluated, so no composition");
        Require(!schedule.Next(Alternate()).evaluate, "skips again once the chain is intact");
        d = schedule.Next(Alternate(false, true));
        Require(d.evaluate && d.composeMotion, "a change after a skip still bridges the model's two-frame history");
    }
    // Freeze frame evaluates every frame and keeps nothing; releasing it starts again.
    {
        ModelRateSchedule schedule;
        schedule.Next(Alternate());
        Require(!schedule.Next(Alternate()).evaluate, "skips before the hold");
        auto d = schedule.Next(Alternate(false, false, true));
        Require(d.evaluate && d.composeMotion && d.captureGuides && !d.storeChange, "first held frame bridges the skip");
        d = schedule.Next(Alternate(false, false, true));
        Require(d.evaluate && !d.composeMotion && !d.captureGuides && !d.storeChange, "held frames always evaluate");
        Require(schedule.Next(Alternate(true)).evaluate, "release evaluates");
        Require(!schedule.Next(Alternate()).evaluate, "then alternates");
    }
    // Switching back to every frame right after a skip still bridges that skip once.
    {
        ModelRateSchedule schedule;
        schedule.Next(Alternate());
        Require(!schedule.Next(Alternate()).evaluate, "skip");
        auto d = schedule.Next({});
        Require(d.evaluate && d.composeMotion && d.captureGuides && !d.storeChange, "switch back composes the skipped frame");
        d = schedule.Next({});
        Require(d.evaluate && !d.composeMotion && !d.captureGuides, "then plain every frame");
        Require(schedule.Next(Alternate()).evaluate, "switching on again evaluates first");
    }
    // Invalidate forgets the stored frame.
    {
        ModelRateSchedule schedule;
        schedule.Next(Alternate());
        schedule.Invalidate();
        Require(schedule.Next(Alternate()).evaluate, "nothing stored after invalidation");
    }
    Require(SanitizeModelRate(static_cast<ModelRate>(7)) == ModelRate::EveryFrame, "unknown rates run every frame");
    // Every third frame is Lab only: a saved 2 runs every frame, and the Lab switch
    // turns only Every other frame into every third frame.
    Require(SanitizeModelRate(ModelRate::EveryThirdFrame) == ModelRate::EveryFrame, "every third frame is never saved");
    Require(EffectiveModelRate(ModelRate::EveryOtherFrame, true) == ModelRate::EveryThirdFrame &&
            EffectiveModelRate(ModelRate::EveryOtherFrame, false) == ModelRate::EveryOtherFrame &&
            EffectiveModelRate(ModelRate::EveryFrame, true) == ModelRate::EveryFrame &&
            EffectiveModelRate(ModelRate::EveryThirdFrame, true) == ModelRate::EveryFrame, "the Lab switch");
    Require(!LabEveryThirdFrame(), "the Lab switch starts off");

    // Which option changes make the stored change stale.
    {
        NeuralOptions base;
        base.enabled = true;
        base.passes = 2;
        auto other = base;
        other.reconstruction.modelRate = ModelRate::EveryOtherFrame;
        Require(SameModelOutputOptions(other, base) && SameHistoryOptions(other, base), "the rate itself changes neither");
        other = base; other.compare.split = true; other.compare.splitPosition = 0.3f;
        Require(SameModelOutputOptions(other, base), "the comparison is drawn after the stored change");
        other = base; other.objectMasks.previewGroups = 3;
        Require(SameModelOutputOptions(other, base), "the mask preview is drawn after the stored change");
        other = base; other.tuning.intensity = 0.5f;
        Require(!SameModelOutputOptions(other, base), "tuning changes NR's output");
        other = base; other.reconstruction.broadStrength = 0.5f;
        Require(!SameModelOutputOptions(other, base) && SameHistoryOptions(other, base),
            "Broad changes the output without touching history");
        other = base; other.reconstruction.method = ResolveMethod::Ratio;
        Require(!SameModelOutputOptions(other, base), "method");
        other = base; other.beforeUpscaling = !base.beforeUpscaling;
        Require(!SameModelOutputOptions(other, base), "placement");
        other = base; other.passOverride = PassOverride::Combat;
        Require(!SameModelOutputOptions(other, base), "combat running Pass 1 alone");
        other = base; other.combat.inCombat = true;
        Require(SameModelOutputOptions(other, base), "a combat policy edit alone changes nothing shown");
        // Weather and time of day blend look settings each frame under one revision.
        auto weather = base; weather.appearanceRevision = 4;
        other = weather; other.tuning.intensity = 0.9f; other.reconstruction.detailStrength = 1.2f;
        other.extraPasses[0].tuning.localToneStrength = 0.8f;
        Require(SameModelOutputOptions(other, weather), "continuous weather blending keeps alternating");
        other.appearanceRevision = 5;
        Require(!SameModelOutputOptions(other, weather), "a new appearance revision evaluates");
    }
    std::puts("PASS: model rate schedule, composition after skips, freeze, rate switches and output-changing options");
}
