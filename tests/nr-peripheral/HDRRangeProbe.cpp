// Manual hardware probe: how each NR colour path treats HDR scene encodings.
// The same image is supplied as SDR sRGB, linear HDR and PQ. Every result is
// returned to the SDR encoding and compared with NR's change to the SDR image.
// Static synthetic scene; no game hooks, display output or pacing is measured.
#include "GPU.h"
#include <filesystem>
#include <fstream>
#include <nvsdk_ngx.h>

namespace {
constexpr unsigned kW = 960, kH = 540, kFrames = 16;
constexpr float kPaperWhiteNits = 203; // BT.2408 reference white for the PQ case.

float Decode(float c) { c = std::clamp(c, 0.f, 1.f); return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float Encode(float c) { c = std::clamp(c, 0.f, 1.f); return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f; }
// Community Shaders' HDR scene: pure 2.2 gamma, 1.0 = paper white, highlights above 1.
float ToGamma(float c) { return std::pow(std::max(c, 0.f), 1 / 2.2f); }
float FromGamma(float c) { return std::pow(std::max(c, 0.f), 2.2f); }
constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
float ToPQ(float linear) {
    const double y = std::clamp(double(linear) * kPaperWhiteNits / 10000, 0.0, 1.0), p = std::pow(y, m1);
    return float(std::pow((c1 + c2 * p) / (1 + c3 * p), m2));
}
float FromPQ(float e) {
    const double p = std::pow(std::clamp(double(e), 0.0, 1.0), 1 / m2);
    return float(std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1 / m1) * 10000 / kPaperWhiteNits);
}
float Hash(unsigned x, unsigned y, unsigned s) {
    unsigned h = x * 374761393u + y * 668265263u + s * 2246822519u; h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffff) / float(0xffffff);
}
float Noise(float x, float y, unsigned s) {
    const auto xi = unsigned(x), yi = unsigned(y); const float fx = x - xi, fy = y - yi;
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Hash(xi, yi, s) + (Hash(xi + 1, yi, s) - Hash(xi, yi, s)) * sx;
    const float b = Hash(xi, yi + 1, s) + (Hash(xi + 1, yi + 1, s) - Hash(xi, yi + 1, s)) * sx;
    return a + (b - a) * sy;
}
// Linear-light scene. SDR content is authored in sRGB and decoded; highlights
// are added in linear light above paper white (1.0).
std::vector<Pixel> Scene() {
    std::vector<Pixel> linear(size_t(kW) * kH);
    for (unsigned y = 0; y < kH; ++y) for (unsigned x = 0; x < kW; ++x) {
        const float u = float(x) / kW, v = float(y) / kH;
        float r, g, b;
        if (v < 0.45f) { // Sky gradient with soft cloud texture.
            const float cloud = std::clamp(Noise(x / 48.f, y / 24.f, 1) * 0.6f + Noise(x / 12.f, y / 8.f, 2) * 0.25f - 0.35f, 0.f, 1.f);
            r = 0.35f + 0.25f * v + 0.4f * cloud; g = 0.52f + 0.2f * v + 0.33f * cloud; b = 0.78f + 0.1f * cloud;
        } else { // Ground: layered colour noise, fine grass-like detail and hard edges.
            const float n = Noise(x / 32.f, y / 32.f, 3) * 0.5f + Noise(x / 6.f, y / 6.f, 4) * 0.3f + Hash(x, y, 5) * 0.2f;
            r = 0.18f + 0.35f * n; g = 0.26f + 0.4f * n; b = 0.12f + 0.2f * n;
            if (x > kW / 10 && x < kW / 3 && y > kH * 6 / 10 && y < kH * 9 / 10) { // Stone wall: bricks and thin mortar.
                const bool mortar = (y % 18) < 2 || ((x + ((y / 18) % 2) * 20) % 40) < 2;
                const float s = 0.45f + 0.25f * Noise(x / 5.f, y / 5.f, 6);
                r = g = b = mortar ? 0.2f : s; r += 0.05f;
            }
            if (x > kW / 2 && x < kW * 6 / 10 && y > kH * 5 / 10) { // Thin diagonal fence lines.
                if ((x + y) % 14 < 2) { r = 0.08f; g = 0.07f; b = 0.06f; }
            }
        }
        Pixel p{ Decode(r), Decode(g), Decode(b), 1 };
        // Sun disc and glow, a warm window and specular points.
        const float dx = x - kW * 0.78f, dy = y - kH * 0.2f, d = std::sqrt(dx * dx + dy * dy);
        const float sun = d < 26 ? 20.f : 6.f * std::exp(-(d - 26) / 18.f);
        const float window = (x > kW * 0.66f && x < kW * 0.74f && y > kH * 0.62f && y < kH * 0.8f) ? 2.5f : 0.f;
        const bool spec = y > kH * 0.55f && x > kW * 0.82f && (x % 23) < 3 && (y % 19) < 3;
        const Pixel light{ sun + window + (spec ? 4.f : 0.f), sun * 0.92f + window * 0.8f + (spec ? 4.f : 0.f), sun * 0.8f + window * 0.5f + (spec ? 4.f : 0.f), 0 };
        for (unsigned c = 0; c < 3; ++c) p[c] = std::max(p[c], light[c]);
        linear[size_t(y) * kW + x] = p;
    }
    return linear;
}
float Peak(const Pixel& p) { return std::max({ p[0], p[1], p[2] }); }

struct Run { std::string name; std::vector<Pixel> output; };

std::vector<Pixel> Evaluate(GPU& gpu, NeuralOptions options, const std::vector<Pixel>& input, DXGI_FORMAT format) {
    auto original = gpu.Texture(kW, kH, input, format), scene = gpu.Texture(kW, kH, {}, format);
    auto motion = gpu.Texture(kW, kH, std::vector<Pixel>(size_t(kW) * kH, {}), DXGI_FORMAT_R32G32_FLOAT);
    auto depth = gpu.Texture(kW, kH, std::vector<Pixel>(size_t(kW) * kH, { .5f, 0, 0, 0 }), DXGI_FORMAT_R32_FLOAT);
    NeuralPass pass;
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        gpu.Begin(); Check(Interop::RecordCopy(gpu.list.Get(), original.Get(), scene.Get()), "fresh scene");
        const bool ok = pass.Record(gpu.device.Get(), gpu.list.Get(), frame % kCommandSlots, options, frame == 0, true,
            float(kW), float(kH), motion.Get(), depth.Get(), nullptr, scene.Get(), nullptr);
        Require(ok, pass.Status().c_str()); gpu.End(); pass.RetireTelemetry();
    }
    return gpu.Read(pass.Corrected()); // Every submission retired before the pass is released.
}
}

int wmain(int argc, wchar_t** argv) {
    Require(argc == 3, "usage: probe absolute-NR-DLL output-directory");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const std::filesystem::path out = argv[2]; std::filesystem::create_directories(out);
    GPU gpu(true);
    const auto cache = std::filesystem::absolute(out / "ngx-cache"); std::filesystem::create_directories(cache);
    const auto init = NVSDK_NGX_D3D12_Init_with_ProjectID("f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "nr-hdr-range-probe", cache.c_str(), gpu.device.Get(), nullptr, NVSDK_NGX_Version_API);
    Require(NVSDK_NGX_SUCCEED(init) || init == NVSDK_NGX_Result_FAIL_FeatureAlreadyExists, "normal NGX bootstrap");

    const auto linear = Scene();
    std::vector<Pixel> sdr(linear.size()), pq(linear.size()), gammaSdr(linear.size()), gamma(linear.size());
    for (size_t i = 0; i < linear.size(); ++i) for (unsigned c = 0; c < 4; ++c) {
        sdr[i][c] = c == 3 ? 1 : Encode(linear[i][c]);
        pq[i][c] = c == 3 ? 1 : ToPQ(linear[i][c]);
        gammaSdr[i][c] = c == 3 ? 1 : ToGamma(std::min(linear[i][c], 1.f));
        gamma[i][c] = c == 3 ? 1 : ToGamma(linear[i][c]);
    }
    // SDR comparison region: pixels at least 12 px from any value above 0.7.
    std::vector<unsigned char> bright(linear.size()), region(linear.size());
    for (size_t i = 0; i < linear.size(); ++i) bright[i] = Peak(linear[i]) > 0.7f;
    for (int y = 0; y < int(kH); ++y) for (int x = 0; x < int(kW); ++x) {
        bool nearby = false;
        for (int j = std::max(0, y - 12); j <= std::min(int(kH) - 1, y + 12) && !nearby; ++j)
            for (int i = std::max(0, x - 12); i <= std::min(int(kW) - 1, x + 12) && !nearby; ++i) nearby = bright[size_t(j) * kW + i];
        region[size_t(y) * kW + x] = !nearby && x >= 16 && y >= 16 && x < int(kW) - 16 && y < int(kH) - 16;
    }

    NeuralOptions base; base.enabled = true; base.runtimePath = argv[1]; base.beforeUpscaling = false; base.worldOnly = true;
    base.tuning.uiCorrection = false;
    auto withRatio = [&](bool hdr, bool producer, float white = 1) {
        auto o = base; o.reconstruction.method = TheosRenderPipeline::NeuralRendering::ResolveMethod::Ratio;
        o.reconstruction.colorIsHDR = hdr; o.reconstruction.producerColor = producer; o.reconstruction.whitePoint = white; return o;
    };
    // Each case: name, options, input, format, conversion of the output back to
    // linear light, and comparison family. The first case of each family is its
    // SDR reference: sRGB for generic sources, 2.2 gamma for Community Shaders.
    struct Case { std::string name; NeuralOptions options; const std::vector<Pixel>* input; DXGI_FORMAT format; int back, family; };
    const std::vector<Case> cases{
        { "sdr-auto-reference", base, &sdr, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0 },
        { "sdr-auto-repeat", base, &sdr, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0 },
        { "linear-auto", base, &linear, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0 },
        { "linear-hdr-ratio", withRatio(true, false), &linear, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0 },
        { "linear-hdr-ratio-w0.5", withRatio(true, false, .5f), &linear, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0 },
        { "linear-hdr-ratio-w2", withRatio(true, false, 2), &linear, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0 },
        { "linear-producer", withRatio(false, true), &linear, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0 },
        { "pq-auto", base, &pq, DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 0 },
        { "cs-sdr-auto-reference", base, &gammaSdr, DXGI_FORMAT_R8G8B8A8_UNORM, 3, 1 },
        { "cs-hdr-auto", base, &gamma, DXGI_FORMAT_R16G16B16A16_FLOAT, 3, 1 },
        { "cs-hdr-producer", withRatio(false, true), &gamma, DXGI_FORMAT_R16G16B16A16_FLOAT, 3, 1 },
    };
    std::array<std::vector<Pixel>, 2> references;
    std::ofstream csv(out / "results.csv");
    csv << "case,region_pixels,nr_change_rms_8bit,delta_error_rms_8bit,relative_delta_error,highlight_pixels,highlight_peak_ratio,highlight_collapsed_fraction\n";
    std::printf("%-22s %10s %12s %10s %12s %10s\n", "case", "NR-rms", "err-vs-ref", "rel-err", "hl-ratio", "collapsed");
    for (const auto& c : cases) {
        const auto result = Evaluate(gpu, c.options, *c.input, c.format);
        std::vector<Pixel> back(result.size());
        for (size_t i = 0; i < result.size(); ++i) for (unsigned ch = 0; ch < 3; ++ch) {
            const float v = result[i][ch];
            Require(std::isfinite(v), "finite NR output");
            back[i][ch] = c.back == 0 ? v : c.back == 1 ? Decode(v) : c.back == 2 ? FromPQ(v) : FromGamma(v);
        }
        std::ofstream raw(out / (c.name + ".f32"), std::ios::binary);
        raw.write(reinterpret_cast<const char*>(back.data()), back.size() * sizeof(Pixel));
        // SDR region: compare in the family's SDR encoding against NR's change to its SDR image.
        auto& referenceDelta = references[c.family];
        const auto& display = c.family ? gammaSdr : sdr;
        double change = 0, error = 0; size_t n = 0;
        std::vector<Pixel> delta(result.size());
        for (size_t i = 0; i < result.size(); ++i) {
            if (!region[i]) continue;
            for (unsigned ch = 0; ch < 3; ++ch) {
                delta[i][ch] = (c.family ? std::min(ToGamma(back[i][ch]), 1.f) : Encode(back[i][ch])) - display[i][ch];
                change += double(delta[i][ch]) * delta[i][ch];
                if (!referenceDelta.empty()) { const double e = delta[i][ch] - referenceDelta[i][ch]; error += e * e; }
            }
            ++n;
        }
        if (referenceDelta.empty()) referenceDelta = delta;
        double referenceChange = 0;
        for (size_t i = 0; i < result.size(); ++i) if (region[i]) for (unsigned ch = 0; ch < 3; ++ch) referenceChange += double(referenceDelta[i][ch]) * referenceDelta[i][ch];
        const double rms = std::sqrt(change / (3.0 * n)) * 255, errorRms = std::sqrt(error / (3.0 * n)) * 255;
        const double relative = std::sqrt(error / std::max(referenceChange, 1e-20));
        // Highlight core: linear peak above 1.5 paper white.
        double inPeak = 0, outPeak = 0; size_t highlights = 0, collapsed = 0;
        for (size_t i = 0; i < linear.size(); ++i) {
            if (Peak(linear[i]) <= 1.5f) continue;
            inPeak += Peak(linear[i]); outPeak += Peak(back[i]); ++highlights; collapsed += Peak(back[i]) < 1.0f;
        }
        const double ratio = outPeak / inPeak, collapsedFraction = double(collapsed) / highlights;
        csv << c.name << ',' << n << ',' << rms << ',' << errorRms << ',' << relative << ',' << highlights << ',' << ratio << ',' << collapsedFraction << '\n';
        std::printf("%-22s %10.3f %12.3f %10.3f %12.4f %10.4f\n", c.name.c_str(), rms, errorRms, relative, ratio, collapsedFraction);
        std::fflush(stdout);
    }
    csv.close();
    Require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get())), "normal NGX shutdown");
    std::puts("COMPLETE");
}
