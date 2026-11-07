#include "FrameGen/SteadyPacing.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <random>

using TheosRenderPipeline::SteadyPacing;
using TheosRenderPipeline::SteadyOutputIntervalUs;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

// A frame that would take `cost` us, held to the controller's current cap.
static double Paced(const SteadyPacing& pacing, double cost)
{
    return pacing.TargetUs() > 0 ? (std::max)(cost, pacing.TargetUs()) : cost;
}

int main()
{
    // Alternating 10 ms and 20 ms frames (a model every other frame) settle on
    // a cap just above 20 ms, and stay there instead of drifting.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 119; ++frame) { pacing.Observe(frame % 2 ? 20000 : 10000); }
        Require(pacing.TargetUs() == 0, "nothing is capped before the first full window");
        for (int frame = 0; frame < 60; ++frame) { pacing.Observe(frame % 2 ? 20000 : 10000); }
        Require(pacing.TargetUs() >= 20000 && pacing.TargetUs() <= 21000, "the first cap sits just above the slow frames");
        double low = 1e9, high = 0;
        for (int frame = 0; frame < 6000; ++frame) {
            pacing.Observe(Paced(pacing, frame % 2 ? 20000 : 10000));
            if (frame > 600) { low = (std::min)(low, pacing.TargetUs()); high = (std::max)(high, pacing.TargetUs()); }
        }
        Require(low >= 19300 && high <= 21300, "the cap holds within a few percent of the slow frames' cost");
    }
    // Heavier scenes raise the cap within a couple of seconds.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 600; ++frame) { pacing.Observe(Paced(pacing, 16000)); }
        const double before = pacing.TargetUs();
        for (int frame = 0; frame < 900; ++frame) { pacing.Observe(Paced(pacing, 22000)); }
        Require(before < 17000 && pacing.TargetUs() * SteadyPacing::kSlowTolerance >= 22000, "a heavier scene backs the cap off");
    }
    // 2026-10-22 game test: while the limiter holds frames to the cap, single
    // intervals land up to 15% either side of it but average to it. The old
    // miss count read that wobble as overload and walked a 28.6 FPS cap down to
    // 21.3. Model every other frame before upscaling: 14 ms and 34 ms frames.
    {
        SteadyPacing pacing;
        constexpr double wobble[]{ 0.15, -0.10, -0.05, 0.08, -0.08 };
        double low = 1e9, high = 0;
        for (int frame = 0; frame < 9000; ++frame) {
            const double cost = frame % 2 ? 34000 : 14000;
            const double held = Paced(pacing, cost);
            pacing.Observe(pacing.TargetUs() > 0 && cost < pacing.TargetUs() ? held * (1 + wobble[frame % 5]) : held);
            if (frame > 1200) { low = (std::min)(low, pacing.TargetUs()); high = (std::max)(high, pacing.TargetUs()); }
        }
        // Probing may dip a few percent under the 34 ms frames before the average notices.
        Require(low >= 32500 && high <= 36500, "limiter wobble does not walk the cap away");
    }
    // 2026-10-22 game test (C7): the cap tried 1% faster, missed, backed off 3%
    // below the cap that had held, and tried again every few seconds. A failed
    // try now returns to the cap that held and waits longer before the next.
    // 14 ms and 34 ms frames: caps below about 33.5 ms cannot hold.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 3000; ++frame) { pacing.Observe(Paced(pacing, frame % 2 ? 34000 : 14000)); }
        int changes = 0;
        double low = 1e9, high = 0;
        for (int frame = 0; frame < 12000; ++frame) {
            changes += pacing.Observe(Paced(pacing, frame % 2 ? 34000 : 14000)) ? 1 : 0;
            low = (std::min)(low, pacing.TargetUs());
            high = (std::max)(high, pacing.TargetUs());
        }
        // Each failed try is two changes (down and back); the waits double from 300 frames.
        Require(changes <= 8, "failed tries back off instead of repeating every few seconds");
        // A try dips 1% below the cap that held; the old back-off then rose 3% above it.
        Require(low >= 32500 && high <= 33500, "a failed try returns to the cap that held, not 3% above it");
    }
    // 2026-11-02 player log (RTX 3060 Laptop, DLSS-G x3, VSync forced on): GPU
    // work stayed near 17 ms, but held frames always arrived later than the
    // cap and VRAM hitches recurred whatever the cap. Every 3% back-off moved
    // the frames 3% later, so the cap walked from 29 to 9 FPS. A back-off that
    // the frames just follow is undone and the excess tolerated instead.
    {
        struct Case { const char* why; double scale, add; int spikeEvery; double spike; };
        constexpr Case cases[]{
            { "a proportional presentation delay does not walk the cap down", 1.05, 0, 0, 0 },
            { "a fixed presentation delay does not walk the cap down", 1.0, 3000, 0, 0 },
            { "hitches that do not depend on the cap do not walk it down", 1.0, 0, 8, 60000 },
        };
        for (const auto& c : cases) {
            SteadyPacing pacing;
            double first = 0, high = 0;
            for (int frame = 0; frame < 20000; ++frame) {
                double interval = Paced(pacing, 17000) * c.scale + c.add;
                if (c.spikeEvery && frame % c.spikeEvery == 0) { interval += c.spike; }
                pacing.Observe(interval);
                if (!first && pacing.TargetUs() > 0) { first = pacing.TargetUs(); }
                high = (std::max)(high, pacing.TargetUs());
            }
            Require(first > 0 && high <= first * 1.07, c.why);
        }
    }
    // 2026-11-02 game test, generation off: GPU frames 22 ms typical, 27 ms at
    // p95. Held to the exact cap, uneven frames keep it near where it started;
    // rounded to whole FPS it walked from 36 to 32.
    {
        std::mt19937 rng(7);
        auto uniform = [&] { return (rng() + 0.5) / 4294967296.0; };
        SteadyPacing pacing;
        double first = 0, low = 1e9;
        for (int frame = 0; frame < 20000; ++frame) {
            const double z = std::sqrt(-2 * std::log(uniform())) * std::cos(6.283185307179586 * uniform());
            const double cost = 22000 * std::exp(0.12 * z);
            const double cap = SteadyOutputIntervalUs(pacing.TargetUs(), 1);
            pacing.Observe(cap ? (std::max)(cost, double(cap)) : cost);
            if (!first && pacing.TargetUs() > 0) { first = pacing.TargetUs(); }
            if (first) { low = (std::min)(low, pacing.TargetFPS()); }
        }
        Require(first > 0 && low >= 1e6 / first * 0.98, "uneven frames without generation do not walk the cap down");
    }
    // A real overload is still followed when a presentation delay was learned.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 3000; ++frame) { pacing.Observe(Paced(pacing, 17000) * 1.05); }
        for (int frame = 0; frame < 3000; ++frame) { pacing.Observe(Paced(pacing, 26000) * 1.05); }
        Require(pacing.TargetUs() * pacing.Allowance() * SteadyPacing::kSlowTolerance >= 26000 * 1.05,
            "a heavier scene still backs the cap off after a delay was learned");
    }
    // 2026-11-02 player log: turning DLSS-G on stalled frames for a few seconds
    // and the first cap measured those stalls (18 FPS on a GPU good for ~50).
    // After a restart the first seconds are not measured.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 600; ++frame) { pacing.Observe(Paced(pacing, 17000)); }
        pacing.Restart();
        for (int frame = 0; frame < 40; ++frame) { pacing.Observe(55000); } // 2.2 s of start-up stalls
        Require(pacing.TargetUs() == 0, "nothing is capped while settling");
        for (int frame = 0; frame < 400; ++frame) { pacing.Observe(Paced(pacing, 20000)); }
        Require(pacing.TargetUs() > 0 && pacing.TargetUs() <= 20000 * SteadyPacing::kHeadroom * 1.01,
            "the first cap after settling measures the new cost, not the start-up stalls");
        Require(!pacing.Observe(900000) && pacing.TargetUs() == 0, "a long pause restarts too");
        for (int frame = 0; frame < 100; ++frame) { pacing.Observe(20000); }
        Require(pacing.TargetUs() == 0, "and settles before measuring again");
    }
    // Lighter scenes are found by probing, slowly.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 600; ++frame) { pacing.Observe(Paced(pacing, 22000)); }
        const double before = pacing.TargetUs();
        for (int frame = 0; frame < 6000; ++frame) { pacing.Observe(Paced(pacing, 14000)); }
        Require(pacing.TargetUs() < before * 0.9, "a lighter scene lets the cap come down");
    }
    // Loading screens and alt-tab start over instead of capping at their gap.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 300; ++frame) { pacing.Observe(16000); }
        Require(pacing.TargetUs() > 0, "measured");
        Require(!pacing.Observe(900000) && pacing.TargetUs() == 0, "a long gap resets the measurement");
        Require(!pacing.Observe(-1) && !pacing.Observe(0), "invalid intervals are ignored");
    }
    // The cap converts to the output limit frame generation's limiters apply.
    Require(SteadyOutputIntervalUs(20000, 1) == 20000, "no generation: rendered interval");
    Require(SteadyOutputIntervalUs(20000, 4) == 5000, "x4: four outputs per rendered frame");
    Require(SteadyOutputIntervalUs(30770, 1) == 30770, "exact, not rounded to a whole FPS (32.5 FPS stays 32.5)");
    Require(SteadyOutputIntervalUs(0, 4) == 0 && SteadyOutputIntervalUs(20000, 0) == 0, "unmeasured or invalid: no cap");
    Require(SteadyOutputIntervalUs(500, 4) == 1000 && SteadyOutputIntervalUs(2e6, 1) == 1000000, "clamped to the limiters' range");
    {
        using TheosRenderPipeline::SourceDLSSG::FrameLimitMode;
        using TheosRenderPipeline::SourceDLSSG::EffectiveFrameLimit;
        static_assert(EffectiveFrameLimit(FrameLimitMode::Steady, true, false) == FrameLimitMode::Off, "Steady is off with XeFG");
        static_assert(EffectiveFrameLimit(FrameLimitMode::Steady, true, true) == FrameLimitMode::Steady, "the Lab can allow it");
        static_assert(EffectiveFrameLimit(FrameLimitMode::Steady, false, false) == FrameLimitMode::Steady, "DLSS-G and no generation keep Steady");
        static_assert(EffectiveFrameLimit(FrameLimitMode::Fixed, true, false) == FrameLimitMode::Fixed, "Fixed caps output with XeFG as before");
    }
    std::puts("Steady pacing: alternating cost, heavier and lighter scenes, limiter wobble, gaps, output conversion and the XeFG rule passed.");
}
