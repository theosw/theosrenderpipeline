#include "GPU.h"
#include "FeatureDouble.h"

// Exercise the production recorder, including both successive carried frames.
// A translating gradient plus an affine nonidentity model exposes a stale
// change texture; an identity model or a still scene would not.
int main()
{
    using namespace TheosRenderPipeline::NeuralRendering;
    GPU gpu;
    for (int route : {0, 1, 2}) for (int passes : {1, 2, 5}) {
        constexpr unsigned w = 64, h = 16, delta = 8;
        const bool before = route == 0, world = route != 2;
        const unsigned gw = before ? w : w / 2, gh = before ? h : h / 2;
        Fixture::peripheral = false; Fixture::worldOnly = world; Fixture::transform = true;
        Fixture::sourceWidth = Fixture::workWidth = w; Fixture::sourceHeight = Fixture::workHeight = h;
        Fixture::guideWidth = gw; Fixture::guideHeight = gh;
        Fixture::secondWidth = w; Fixture::secondHeight = h;
        Fixture::passWidth.fill(0); Fixture::passHeight.fill(0);
        Fixture::intensity.fill(.8f); Fixture::networkPreset.fill(-1);
        Fixture::creations = Fixture::evaluations = Fixture::resets = 0;
        Fixture::lateCreationFrame = ~0u; Fixture::motions.clear();
        NeuralOptions options; options.enabled = true; options.beforeUpscaling = before;
        options.worldOnly = world; options.passes = passes; options.runtimePath = "scripted.dll";
        options.tuning.intensity = .8f; options.tuning.uiCorrection = !world;
        options.reconstruction.modelRate = ModelRate::EveryOtherFrame;
        LabEveryThirdFrame() = true;
        auto depth = gpu.Texture(gw, gh, std::vector<Pixel>(gw * gh, {.4f, 0, 0, 0}), DXGI_FORMAT_R32_FLOAT);
        auto moving = gpu.Texture(gw, gh, std::vector<Pixel>(gw * gh, {float(delta) / w, 0, 0, 0}), DXGI_FORMAT_R32G32_FLOAT);
        auto ui = gpu.Texture(w, h, std::vector<Pixel>(w * h, {.01f, .02f, .03f, .25f}));
        auto composed = gpu.Texture(w, h), scene = gpu.Texture(w, h);
        std::array<std::vector<Pixel>, 3> pixels;
        std::array<ComPtr<ID3D12Resource>, 3> originals;
        for (unsigned f = 0; f < 3; ++f) {
            pixels[f].resize(w * h);
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
                const float p = float(x + delta * f);
                pixels[f][y * w + x] = {.02f + .008f * p, .15f + .004f * p, .1f + .002f * p, .6f};
            }
            originals[f] = gpu.Texture(w, h, pixels[f]);
        }
        UINT64 frequency{}; Check(gpu.queue->GetTimestampFrequency(&frequency), "timestamp frequency");
        NeuralPass pass;
        unsigned serial = 0, modelSamples = 0;
        auto record = [&](unsigned content, bool reset) {
            Fixture::frame = serial; const auto evaluations = Fixture::evaluations;
            gpu.Begin(); Check(Interop::RecordCopy(gpu.list.Get(), originals[content].Get(), scene.Get()), "fresh scene");
            Require(pass.Record(gpu.device.Get(), gpu.list.Get(), serial++ % kCommandSlots, options, reset, true, float(gw), float(gh),
                moving.Get(), depth.Get(), world ? nullptr : ui.Get(), scene.Get(), world ? nullptr : composed.Get(), frequency),
                pass.Status().c_str());
            gpu.End();
            modelSamples += Fixture::evaluations != evaluations;
            return Fixture::evaluations - evaluations;
        };
        // Warm every feature/kernel, including deferred creation, before testing
        // the recorder's real 100 ms gap invalidation policy.
        for (unsigned f = 0; f < 9; ++f) { record(0, true); }
        // Also warm skip-only PSOs (reprojection and accumulated motion), so
        // first-use compilation cannot trip the production gap reset.
        record(0, true); record(1, false); record(2, false);
        const auto telemetryBefore = pass.Telemetry().gpuSamples;
        const auto framesBefore = pass.Telemetry().frameSamples;
        for (unsigned f = 0; f < 3; ++f) {
            const auto ran = record(f, f == 0);
            Require(ran == (f == 0 ? unsigned(passes) : 0), "evaluate then two skips, across the whole chain");
            const auto result = gpu.Read(pass.Corrected());
            double maximum = 0;
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w - 2 * delta; ++x) {
                const auto i = y * w + x;
                for (unsigned c = 0; c < 3; ++c) {
                    double expected = pixels[f][i][c];
                    for (int p = 0; p < passes; ++p) { expected = .8 * expected + (p ? .02 : .01); }
                    maximum = std::max(maximum, std::abs(double(result[i][c]) - expected));
                }
                Near(result[i][3], pixels[f][i][3], 0, "current alpha survives both skips");
            }
            std::printf("route=%d passes=%d phase=%u max_error=%.9f\n", route, passes, f, maximum);
            Require(maximum < .001, "both skipped frames carry the change along the translating scene");
        }
        pass.RetireTelemetry(); // GPU fixture already retired every list.
        const auto timing = pass.Telemetry();
        Require(timing.frameSamples > framesBefore && timing.gpuSamples > telemetryBefore,
            "whole-frame and inference timestamps are harvested independently");
        Require(timing.frameSamples == serial && timing.gpuSamples == modelSamples,
            "whole-frame timing includes both skips; inference timing excludes them");
        Require(timing.frameTicksTotal > timing.gpuTicksTotal && !timing.frameQueryFailures && !timing.gpuQueryFailures,
            "total span includes model plus preparation/reprojection/storage, with valid timestamps");
        // Resets, changing output settings and switching rates must not reuse
        // the previous two-skip result out of turn.
        Require(record(0, true) == unsigned(passes), "reset forces evaluation");
        Require(record(1, false) == 0, "first skip after reset");
        options.tuning.intensity = .7f; Fixture::intensity.fill(.7f);
        Require(record(2, false) == unsigned(passes), "tuning change forces evaluation on the second skip");
        options.reconstruction.modelRate = ModelRate::EveryFrame;
        Require(record(0, false) == unsigned(passes), "switch to every frame evaluates immediately");
        LabEveryThirdFrame() = false; options.reconstruction.modelRate = ModelRate::EveryOtherFrame;
        Require(record(0, true) == unsigned(passes), "restart every other frame");
        Require(record(1, false) == 0 && record(2, false) == unsigned(passes), "ordinary every other frame remains alternating");
        pass.RetireTelemetry();
    }
    LabEveryThirdFrame() = false;
    std::puts("PASS: third-frame production pixels, 1/2/5 passes, both placements, native UI, invalidation, live rates and retired timing");
}
