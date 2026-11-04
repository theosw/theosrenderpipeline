#pragma once
// Model rate: every other frame. The production capture, composition, store
// and reprojection kernels on WARP, then the production recorder alternating
// with the scripted model.
#include <format>

namespace RateFixture {
// The CPU form of the stored change, applied to another input.
inline float Carry(float stored, float storedInput, float input, float floor)
{
    const float ratio = std::clamp((stored + floor) / (storedInput + floor), 1.f / 64, 64.f);
    return (input + floor) * ratio - floor;
}
struct Kernels {
    GPU& gpu; NeuralResolveKernels kernels; unsigned stage{};
    explicit Kernels(GPU& g) : gpu(g) { Check(kernels.Initialize(gpu.device.Get()), "model-rate kernels"); }
    void Run(ResolveKernel kernel, const ResolveConstants& c, ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* original,
        ID3D12Resource* output, ID3D12Resource* fourth = nullptr)
    {
        gpu.Begin();
        Check(kernels.Record(gpu.device.Get(), gpu.list.Get(), 0, NeuralResolveKernels::ModelRateStage(stage++ % 4), kernel, c, a, b,
            original, output, nullptr, fourth), "model-rate dispatch");
        gpu.End();
    }
};
}

static void ModelRateKernels(GPU& gpu)
{
    using RateFixture::Carry;
    RateFixture::Kernels k(gpu);
    unsigned cases = 0;
    // Before upscaling: colour and guides share the jittered render raster. After
    // upscaling: twice the guide size, the guides jittered against it.
    for (bool after : { false, true }) for (bool inverted : { true, false }) {
        const unsigned gw = 32, gh = 20, w = after ? 64 : 32, h = after ? 40 : 20;
        const float s = float(w) / gw;
        ResolveConstants c;
        c.sourceWidth = c.targetWidth = w; c.sourceHeight = c.targetHeight = h;
        c.guideWidth = gw; c.guideHeight = gh; c.motionScaleX = float(gw); c.motionScaleY = float(gh);
        c.passthrough = 1; c.whitePoint = 1; c.changeFloor = .02f; c.guidesJittered = after; c.depthInverted = inverted;
        // A smooth scene, so brightness agrees between neighbouring taps, and a
        // change that varies across it, so a misplaced change shows.
        auto scene = [&](float shift) {
            std::vector<Pixel> v(size_t(w) * h);
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x)
                v[y * w + x] = { .30f + .2f * (x + shift) / w, .25f + .1f * y / h, .4f, .6f };
            return v;
        };
        auto changed = [&](const std::vector<Pixel>& input) {
            auto v = input;
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x)
                for (unsigned ch = 0; ch < 3; ++ch) v[y * w + x][ch] = input[y * w + x][ch] * (.7f + .6f * x / w) + .05f * ch;
            return v;
        };
        const float nearDepth = inverted ? .4f : .6f, farDepth = inverted ? .04f : .96f;
        auto depthOf = [&](bool split) {
            std::vector<Pixel> v(size_t(gw) * gh, Pixel{ nearDepth, 0, 0, 0 });
            if (split) for (unsigned y = 0; y < gh; ++y) for (unsigned x = gw / 2; x < gw; ++x) v[y * gw + x][0] = farDepth;
            return v;
        };
        auto uniform = [&](float px) { return std::vector<Pixel>(size_t(gw) * gh, Pixel{ px / gw, 0, 0, 0 }); };
        auto guidesA = gpu.Texture(gw, gh, {}, DXGI_FORMAT_R16G16B16A16_FLOAT), guidesB = gpu.Texture(gw, gh, {}, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto change = gpu.Texture(w, h, {}, DXGI_FORMAT_R16G16B16A16_FLOAT), out = gpu.Texture(w, h);
        auto depth = gpu.Texture(gw, gh, depthOf(false), DXGI_FORMAT_R32_FLOAT);

        // 1. A still scene reproduces the stored change.
        const auto inputA = scene(0), resultA = changed(inputA);
        auto a = gpu.Texture(w, h, inputA), ra = gpu.Texture(w, h, resultA);
        auto still = gpu.Texture(gw, gh, uniform(0), DXGI_FORMAT_R16G16_FLOAT);
        k.Run(ResolveKernel::CaptureGuides, c, still.Get(), depth.Get(), nullptr, guidesA.Get());
        k.Run(ResolveKernel::StoreChange, c, a.Get(), ra.Get(), nullptr, change.Get());
        k.Run(ResolveKernel::CaptureGuides, c, still.Get(), depth.Get(), nullptr, guidesB.Get());
        k.Run(ResolveKernel::Reproject, c, a.Get(), change.Get(), guidesB.Get(), out.Get(), guidesA.Get());
        auto actual = gpu.Read(out.Get());
        for (size_t i = 0; i < actual.size(); ++i) {
            for (unsigned ch = 0; ch < 3; ++ch) Near(actual[i][ch], resultA[i][ch], .0015, "a still frame reproduces the stored change");
            Near(actual[i][3], inputA[i][3], 0, "alpha is this frame's");
        }

        // 2. Translated by 3 guide pixels with known motion, plus a jitter
        // difference: the change lands where the surface moved to. Before
        // upscaling the colour raster moves by the jitter difference too.
        const float jitterA = after ? .25f : .5f, jitterB = after ? -.25f : -.5f;
        const float shift = 3 * s + (after ? 0 : jitterA - jitterB); // in source pixels
        const auto inputB = scene(shift);
        auto b = gpu.Texture(w, h, inputB);
        auto moving = gpu.Texture(gw, gh, uniform(3), DXGI_FORMAT_R16G16_FLOAT);
        c.cameraJitterX = jitterB; c.previousJitterX = jitterA;
        k.Run(ResolveKernel::CaptureGuides, c, moving.Get(), depth.Get(), nullptr, guidesB.Get());
        k.Run(ResolveKernel::Reproject, c, b.Get(), change.Get(), guidesB.Get(), out.Get(), guidesA.Get());
        actual = gpu.Read(out.Get());
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x + unsigned(shift) + 1 < w; ++x) {
            const auto from = y * w + x + unsigned(shift);
            for (unsigned ch = 0; ch < 3; ++ch)
                Near(actual[y * w + x][ch], Carry(resultA[from][ch], inputA[from][ch], inputB[y * w + x][ch], .02f), .002,
                    "the change follows motion and the jitter difference");
        }
        c.cameraJitterX = c.previousJitterX = 0;

        // 3. Depth rejection: the right half was a distant surface on the stored
        // frame. Taps there are rejected; near the edge the nearest matching tap
        // stands in, further in the input shows unchanged.
        auto splitDepth = gpu.Texture(gw, gh, depthOf(true), DXGI_FORMAT_R32_FLOAT);
        k.Run(ResolveKernel::CaptureGuides, c, still.Get(), splitDepth.Get(), nullptr, guidesA.Get());
        k.Run(ResolveKernel::CaptureGuides, c, still.Get(), depth.Get(), nullptr, guidesB.Get());
        k.Run(ResolveKernel::Reproject, c, a.Get(), change.Get(), guidesB.Get(), out.Get(), guidesA.Get());
        actual = gpu.Read(out.Get());
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
            const auto i = y * w + x;
            const float gx = (x + .5f) / s;
            if (gx < gw / 2 - 1)
                for (unsigned ch = 0; ch < 3; ++ch) Near(actual[i][ch], resultA[i][ch], .0015, "matching depth keeps the change");
            else if (gx >= gw / 2 + 4)
                for (unsigned ch = 0; ch < 4; ++ch) Near(actual[i][ch], inputA[i][ch], 0, "a rejected tap with no match nearby leaves the input");
            else if (gx >= gw / 2 && gx < gw / 2 + 2)
                Require(std::abs(actual[i][2] - inputA[i][2]) > .05f, "a revealed pixel next to the surface takes its change");
        }

        // 4. Object groups reject a tap when both frames classified objects.
        std::vector<Pixel> groups(size_t(gw) * gh);
        for (unsigned i = 0; i < groups.size(); ++i)
            groups[i][0] = TheosRenderPipeline::NeuralRendering::PackObjectPixel(i % gw < gw / 2 ? TheosRenderPipeline::NeuralRendering::ObjectGroup::Creature : TheosRenderPipeline::NeuralRendering::ObjectGroup::World, nearDepth);
        auto mask = gpu.Texture(gw, gh, groups, DXGI_FORMAT_R32_FLOAT);
        c.categories = 1;
        k.Run(ResolveKernel::CaptureGuides, c, still.Get(), depth.Get(), mask.Get(), guidesA.Get());
        k.Run(ResolveKernel::CaptureGuides, c, still.Get(), depth.Get(), nullptr, guidesB.Get());
        k.Run(ResolveKernel::Reproject, c, a.Get(), change.Get(), guidesB.Get(), out.Get(), guidesA.Get());
        actual = gpu.Read(out.Get());
        Near(actual[(h / 2) * w + 1][0], inputA[(h / 2) * w + 1][0], 0, "another object group is rejected");
        Near(actual[(h / 2) * w + w - 2][0], resultA[(h / 2) * w + w - 2][0], .0015, "the same group keeps the change");
        c.categories = 0;
        ++cases;
    }

    // Motion composition: mv2(p) = mv(p) + mv'(p + mv(p)), with the jitter
    // difference, in the raw units; a surface the skipped frame did not show
    // points off screen.
    for (bool inverted : { true, false }) {
        const unsigned gw = 40, gh = 16;
        ResolveConstants c;
        c.guideWidth = gw; c.guideHeight = gh; c.motionScaleX = float(gw); c.motionScaleY = -float(gh);
        c.sourceWidth = gw; c.sourceHeight = gh; c.depthInverted = inverted;
        c.cameraJitterX = -.3f; c.previousJitterX = .3f;
        std::vector<Pixel> skippedMotion(size_t(gw) * gh), currentMotion(size_t(gw) * gh, Pixel{ 3.f / gw, -1.f / gh, 0, 0 });
        for (unsigned y = 0; y < gh; ++y) for (unsigned x = 0; x < gw; ++x) skippedMotion[y * gw + x] = { .1f * x / gw, .05f * y / -float(gh), 0, 0 };
        const float nearDepth = inverted ? .4f : .6f, farDepth = inverted ? .04f : .96f;
        std::vector<Pixel> skippedDepth(size_t(gw) * gh, Pixel{ nearDepth, 0, 0, 0 }), currentDepth = skippedDepth;
        for (unsigned y = 0; y < gh; ++y) for (unsigned x = 30; x < gw; ++x) skippedDepth[y * gw + x][0] = farDepth;
        auto ms = gpu.Texture(gw, gh, skippedMotion, DXGI_FORMAT_R32G32_FLOAT), mc = gpu.Texture(gw, gh, currentMotion, DXGI_FORMAT_R32G32_FLOAT);
        auto ds = gpu.Texture(gw, gh, skippedDepth, DXGI_FORMAT_R32_FLOAT), dc = gpu.Texture(gw, gh, currentDepth, DXGI_FORMAT_R32_FLOAT);
        auto gs = gpu.Texture(gw, gh, {}, DXGI_FORMAT_R16G16B16A16_FLOAT), gc = gpu.Texture(gw, gh, {}, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto composed = gpu.Texture(gw, gh, {}, DXGI_FORMAT_R32G32_FLOAT);
        k.Run(ResolveKernel::CaptureGuides, c, ms.Get(), ds.Get(), nullptr, gs.Get());
        k.Run(ResolveKernel::CaptureGuides, c, mc.Get(), dc.Get(), nullptr, gc.Get());
        k.Run(ResolveKernel::ComposeMotion, c, mc.Get(), gc.Get(), gs.Get(), composed.Get());
        const auto actual = gpu.Read(composed.Get());
        for (unsigned y = 1; y + 2 < gh; ++y) for (unsigned x = 0; x < gw; ++x) {
            const auto i = y * gw + x;
            // Guide pixels: current (3, 1) down-right in pixels, jitter difference +0.6 in x.
            const int qx = int(std::floor(x + .5f + 3 + .6f)), qy = int(std::floor(y + .5f + 1));
            if (qx < 0 || qx >= int(gw)) { Near(actual[i][0], 2.0, 1e-5, "beyond the skipped frame points off screen"); continue; }
            if (qx >= 30) { Near(actual[i][0], 2.0, 1e-5, "a surface the skipped frame did not show points off screen"); continue; }
            Near(actual[i][0] * gw, 3 + .1 * qx, .01, "x composes this frame's and the skipped frame's motion");
            Near(actual[i][1] * -float(gh), 1 + .05 * qy, .01, "y composes with a negative motion scale");
        }
        ++cases;
    }
    std::printf("PASS: %u model-rate kernel cases; still scene, motion and jitter, depth/group rejection and nearest match, composed motion\n", cases);
}

// The production recorder at every other frame: the scripted model runs on
// alternate frames only, a skipped frame reproduces the previous result on a
// still scene, the evaluation after a skip reads composed motion, and a reset
// or a setting change evaluates. The debug layer checks every state transition.
static void ModelRatePasses(GPU& gpu)
{
    using namespace TheosRenderPipeline::NeuralRendering;
    unsigned cases = 0;
    for (int route : { 0, 1, 2, 3 }) for (int passes : { 1, 2 }) for (int method : { 0, 1, 2 }) for (bool peripheral : { false, true }) {
        const bool world = route != 0, producer = route == 2, transform = route < 2;
        if (producer && method != 2) { continue; }
        const unsigned w = 48, h = 30, gw = 24, gh = 15;
        NeuralOptions options; options.enabled = true; options.runtimePath = "scripted.dll";
        options.beforeUpscaling = route == 1 || route == 2; options.worldOnly = route >= 2; options.passes = passes;
        options.tuning.uiCorrection = !world; options.tuning.intensity = .8f;
        auto& r = options.reconstruction;
        r.producerColor = producer; r.peripheralCompression = peripheral; r.modelRate = ModelRate::EveryOtherFrame;
        r.method = method == 2 ? ResolveMethod::Ratio : ResolveMethod::Auto; r.inputScale = method == 1 ? .5f : 1.f;
        options.extraPasses[0].tuning.uiCorrection = !world;
        Fixture::peripheral = peripheral; Fixture::worldOnly = world; Fixture::sourceWidth = w; Fixture::sourceHeight = h;
        Fixture::guideWidth = gw; Fixture::guideHeight = gh; Fixture::transform = transform;
        Fixture::workWidth = ModelExtent(w, r); Fixture::workHeight = ModelExtent(h, r);
        Fixture::secondWidth = Fixture::workWidth; Fixture::secondHeight = Fixture::workHeight;
        Fixture::intensity = { .8f, .8f, .8f, .8f, .8f }; Fixture::networkPreset = { -1, -1, -1, -1, -1 };
        Fixture::creations = Fixture::evaluations = Fixture::resets = 0; Fixture::lateCreationFrame = ~0u;
        Fixture::motions.clear();
        auto motion = gpu.Texture(gw, gh, std::vector<Pixel>(gw * gh, {}), DXGI_FORMAT_R32G32_FLOAT);
        auto depth = gpu.Texture(gw, gh, std::vector<Pixel>(gw * gh, { .4f, 0, 0, 0 }), DXGI_FORMAT_R32_FLOAT);
        auto ui = gpu.Texture(w, h, std::vector<Pixel>(w * h, { .05f, .025f, 0, .25f })), composed = gpu.Texture(w, h);
        NeuralPass pass;pass.SetRateClock(&TestClock::Now);
        std::vector<Pixel> evaluated, evaluatedInput;
        std::vector<bool> ran;
        for (unsigned frame = 0; frame < 9; ++frame) {
            Fixture::frame = frame;
            // A reset on frame 5 and a tuning change on frame 7 evaluate out of turn.
            options.tuning.intensity = frame >= 7 ? .7f : .8f;
            // Linked Pass 2 follows Pass 1's tuning.
            Fixture::intensity.fill(options.tuning.intensity);
            // A non-uniform scene before upscaling would move with the jitter.
            const std::array<float, 2> jitter = route == 2 ? std::array<float, 2>{} :
                std::array<float, 2>{ frame % 2 ? .25f : -.25f, frame % 2 ? -.125f : .125f };
            // The scene brightens a little each frame, so a skipped frame must
            // carry the change onto its own input rather than repeat the last result.
            auto pixels = transform ? std::vector<Pixel>(w * h, { .2f, .3f, .4f, .5f }) : Pattern(w, h, producer);
            for (auto& p : pixels) { p[0] += .01f * frame; }
            auto scene = gpu.Texture(w, h, pixels);
            const auto before = Fixture::evaluations;
            gpu.Begin();
            Require(pass.Record(gpu.device.Get(), gpu.list.Get(), frame % kCommandSlots, options, frame == 0 || frame == 5, true, float(gw), float(gh),
                motion.Get(), depth.Get(), world ? nullptr : ui.Get(), scene.Get(), world ? nullptr : composed.Get(), 0, {}, nullptr,
                options.beforeUpscaling ? std::array<float, 2>{} : jitter, options.beforeUpscaling ? jitter : std::array<float, 2>{}),
                pass.Status().c_str());
            gpu.End();
            ran.push_back(Fixture::evaluations != before);
            Require(Fixture::evaluations == before || Fixture::evaluations == before + unsigned(passes), "every pass runs or none does");
            auto result = gpu.Read(pass.Corrected());
            if (ran.back()) { evaluated = result; evaluatedInput = pixels; }
            else {
                for (size_t i = 0; i < result.size(); ++i) for (unsigned ch = 0; ch < 4; ++ch) {
                    const float input = pixels[i][ch];
                    const float expected = ch == 3 || input < 0 ? input : RateFixture::Carry(evaluated[i][ch], evaluatedInput[i][ch], input, .02f);
                    Near(result[i][ch], expected, .002 * std::max(1.f, std::abs(expected)),
                        "a skipped frame of a still scene carries the previous change onto its input");
                }
                Require(!transform || std::abs(result[0][0] - evaluated[0][0]) > .005f, "the skipped frame is not the previous result");
            }
            if (!world) {
                auto tag = gpu.Read(scene.Get());
                for (size_t i = 0; i < result.size(); ++i) for (unsigned ch = 0; ch < 4; ++ch)
                    Near(tag[i][ch], result[i][ch], 0, "skipped frames feed the same real and FG scene");
            }
            Require(pass.Status().find("model every other frame") != std::string::npos, "status names the model rate");
        }
        // 0 evaluates, 1 skips, 2 composes; 5 resets; 7 changes tuning.
        Require(ran == std::vector<bool>{ true, false, true, false, true, true, false, true, false }, "alternation with forced evaluations");
        // After a skip (frames 2, 4 and the tuning change on 7) the model reads
        // composed motion; otherwise the raw motion.
        const unsigned perFrame = unsigned(passes);
        Require(Fixture::motions.size() == 5 * perFrame, "one motion input per evaluation");
        for (std::size_t e = 0; e < Fixture::motions.size(); ++e) {
            const auto frame = std::array<unsigned, 5>{ 0, 2, 4, 5, 7 }[e / perFrame];
            const bool composedMotion = frame == 2 || frame == 4 || frame == 7;
            Require(peripheral || (Fixture::motions[e] != motion.Get()) == composedMotion, "composed motion after a skipped frame only");
        }
        Require(!pass.NeedsRecreation(options, gw, gh), "the model rate never recreates NR");
        auto every = options; every.reconstruction.modelRate = ModelRate::EveryFrame;
        Require(!pass.NeedsRecreation(every, gw, gh), "switching the rate is live");
        ++cases;
    }
    Fixture::secondWidth = Fixture::secondHeight = 0; Fixture::transform = false; Fixture::frame = 0;
    Fixture::intensity = { .8f, .6f, .6f, .6f, .6f }; Fixture::networkPreset = { -1, 1, 1, 1, 1 };
    std::printf("PASS: %u model-rate recorder cases; routes, passes, Auto/Residual/Ratio, peripheral, alternation, resets and composed motion\n", cases);
}
