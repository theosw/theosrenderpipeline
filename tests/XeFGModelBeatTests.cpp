#include "FrameGen/XeFGModelBeat.h"
#include <cstdio>
#include <cstdlib>
using namespace TheosRenderPipeline;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); } }
int main()
{
    XeFGModelBeat beat;
    double now = 1;
    for (unsigned i = 0; i < 33; ++i) {
        now += i % 2 ? 18000 : 40000;
        const auto d = beat.Plan(now, i % 2 ? NRModelActivity::Skipped : NRModelActivity::Ran);
        Require(d.paced == (i == 32), "warmup uses sixteen complete pairs");
        if (i == 32) Require(d.targetUs == 29000, "pair average keeps the 34 FPS budget, not the 25 FPS slow-frame budget");
        beat.Released(d.deadlineUs);
    }
    // Pending GPU work can run during the hold before Present/retirement.
    // This does not assert GPU completion or screen cadence.
    for (unsigned i = 33; i < 133; ++i) {
        now += 18000;
        const auto d = beat.Plan(now, i % 2 ? NRModelActivity::Skipped : NRModelActivity::Ran);
        Require(std::abs(d.deadlineUs - now - 11000) < .01, "queued work overlaps the hold");
        now = d.deadlineUs; beat.Released(now);
    }
    auto d = beat.Plan(now + 45000, NRModelActivity::Skipped);
    Require(d.deadlineUs == now + 45000, "late work never creates a catch-up burst");
    Require(d.targetUs == 29000, "a single late frame does not lower FPS permanently");
    beat.Released(d.deadlineUs);
    beat.Plan(now + 63000, NRModelActivity::Ran);
    Require(beat.TargetUs() == 29000, "a single late pair does not lower FPS permanently");
    d = beat.Plan(now + 300000, NRModelActivity::Ran);
    Require(!d.paced, "loading gap retrains");
    d = beat.Plan(now + 310000, NRModelActivity::Ran);
    Require(!d.paced, "forced model runs retrain instead of assuming parity");
    Require(!beat.Plan(now + 320000, NRModelActivity::Unknown).paced, "inactive NR cannot pace");
    Require(!beat.Plan(NAN, NRModelActivity::Ran).paced, "bad clocks cannot pace");
    beat.Reset(); now = 1;
    for (unsigned i = 0; i < 33; ++i) {
        now += i % 2 ? 10000 : 12400;
        beat.Plan(now, i % 2 ? NRModelActivity::Skipped : NRModelActivity::Ran);
    }
    Require(std::abs(beat.TargetUs() - 11200) < .01, "small RTX model costs keep their pair budget");
    beat.Reset(); Require(beat.TargetUs() == 0, "turning off discards the learned target");
    // A cheap model hidden under other work must not acquire an unnecessary cap.
    // Uncorrelated CPU jitter also cannot masquerade as a model/skip beat.
    now = 1;
    constexpr double jitter[]{19000, 20000, 21000, 20500, 19500};
    for (unsigned i = 0; i < 120; ++i) {
        now += jitter[i % 5];
        Require(!beat.Plan(now, i % 2 ? NRModelActivity::Skipped : NRModelActivity::Ran).paced,
            "already even frames and small unrelated jitter are not capped");
    }
    Require(beat.PairSamples() == 16 && beat.PhaseDifferenceUs() < 600, "absence of a phase signal is measured, not warmup");
    for (unsigned i = 0; i < 120; ++i) {
        now += i % 2 ? 18000 : 40000;
        beat.Plan(now, i % 2 ? NRModelActivity::Skipped : NRModelActivity::Ran);
    }
    Require(beat.TargetUs() > 28000 && beat.TargetUs() <= 29000, "persistent new alternation activates the beat");
    std::puts("PASS pair calibration, overlapping hold, misses, resets and unavailable activity");
}
