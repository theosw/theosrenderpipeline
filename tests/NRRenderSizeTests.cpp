#include "FrameGen/NRRenderSize.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::NeuralRendering;
void Require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); } }
bool Same(FrameExtent a, FrameExtent b) { return a.width == b.width && a.height == b.height; }
std::vector<std::string> Fields(std::string line)
{
    std::vector<std::string> fields; std::stringstream stream(line); std::string value;
    while (std::getline(stream, value, ',')) { fields.push_back(value); }
    return fields;
}
int main(int argc, char** argv)
{
    Require(argc == 3, "model.py vectors and selection fixtures required");
    std::ifstream vectors(argv[1]); Require(vectors.good(), "open sizing vectors");
    std::string line; std::getline(vectors, line); unsigned count = 0;
    while (std::getline(vectors, line)) {
        auto f = Fields(line); Require(f.size() == 7, "sizing vector format");
        const auto n = [&](unsigned i) { return static_cast<UINT>(std::stoul(f[i])); };
        const auto s = NRInternalSize({n(0), n(1)});
        Require(s.width == n(2) && s.height == n(3) && s.quantumWidth == n(4) && s.quantumHeight == n(5) && s.tokens == n(6), "model.py sizing parity"); ++count;
    }
    std::printf("PASS %u model.py sizing vectors (includes 2872 host-constructor calls)\n", count);
    std::ifstream cases(argv[2]); Require(cases.good(), "open selection cases");
    std::getline(cases, line); count = 0;
    while (std::getline(cases, line)) {
        auto f = Fields(line); Require(f.size() == 12, "selection case format");
        const auto n = [&](unsigned i) { return static_cast<UINT>(std::stoul(f[i])); };
        const FrameExtent output{n(2), n(3)}, nominal{n(4), n(5)}, expected{n(10), n(11)};
        const UpscalerInputResolution range{nominal, {n(6), n(7)}, {n(8), n(9)}};
        const auto quality = f[1] == "Q" ? 2 : f[1] == "B" ? 1 : f[1] == "P" ? 0 : 5;
        const RenderSizePolicy on{true, true, true, quality};
        const auto chosen = SelectNRFriendlyRenderSize(nominal, output, range, on);
        std::printf("%s %s %ux%u: %ux%u -> %ux%u\n", f[0].c_str(), f[1].c_str(), output.width, output.height, nominal.width, nominal.height, chosen.width, chosen.height);
        Require(Same(chosen, expected), "independent exhaustive selection oracle");
        Require(range.Accepts(chosen, output), "SDK range and existing aspect check");
        Require(NRInternalSize(chosen).Area() <= NRInternalSize(nominal).Area(), "never increase NR internal area");
        Require(std::uint64_t(chosen.width) * chosen.height * 100 >= std::uint64_t(nominal.width) * nominal.height * 97, "97 percent nominal pixels");
        for (auto off : {RenderSizePolicy{false, true, true, quality}, RenderSizePolicy{true, false, true, quality},
            RenderSizePolicy{true, true, false, quality}, RenderSizePolicy{true, true, true, 5}}) {
            Require(Same(SelectNRFriendlyRenderSize(nominal, output, range, off), nominal), "switch off, RTX/nonresearch, non-NR/after-upscale, Native unchanged");
        }
        ++count;
    }
    const FrameExtent nominal{753, 424}, output{1280, 720};
    const RenderSizePolicy on{true, true, true, 2};
    const UpscalerInputResolution fixed{nominal, nominal, nominal};
    Require(Same(SelectNRFriendlyRenderSize(nominal, output, fixed, on), nominal), "range refuses all moves");
    Require(Same(SelectNRFriendlyRenderSize(nominal, output, {}, on), nominal), "unavailable range keeps nominal");
    const UpscalerInputResolution badAspect{{753,425},{753,425},{753,425}};
    Require(!badAspect.Accepts({753,425}, output) && Same(SelectNRFriendlyRenderSize(nominal, output, badAspect, on), nominal), "range cannot authorize aspect refusal");
    Require(NRInternalSize({0, 424}).Area() == 0, "invalid dimension");
    // Check the exhaustive optimizer against a deliberately simple Cartesian
    // oracle, including all five scaled presets and adversarial narrow ranges.
    for (unsigned seed = 0; seed < 80; ++seed) {
        const FrameExtent out{640 + seed, 360 + seed / 2};
        const FrameExtent base{out.width / 2, out.height / 2};
        const UpscalerInputResolution range{base, {base.width - 8, base.height - 8}, {base.width + 30, base.height + 30}};
        auto best = base;
        const auto better = [&](FrameExtent a, FrameExtent b) {
            const auto ai = NRInternalSize(a).Area(), bi = NRInternalSize(b).Area();
            if (ai != bi) { return ai < bi; }
            const auto ap = std::uint64_t(a.width) * a.height, bp = std::uint64_t(b.width) * b.height;
            if (ap != bp) { return ap > bp; }
            const auto aspect = [&](FrameExtent s) { return std::abs(double(s.width) / s.height - double(out.width) / out.height); };
            if (aspect(a) != aspect(b)) { return aspect(a) < aspect(b); }
            const auto delta = [&](FrameExtent s) { return std::abs(int(s.width) - int(base.width)) + std::abs(int(s.height) - int(base.height)); };
            return delta(a) < delta(b);
        };
        for (UINT w = range.minimum.width; w <= range.maximum.width; ++w) {
            for (UINT h = range.minimum.height; h <= range.maximum.height; ++h) {
                FrameExtent candidate{w,h};
                if (range.Accepts(candidate, out) && std::uint64_t(w)*h*100 >= std::uint64_t(base.width)*base.height*97 &&
                    NRInternalSize(candidate).Area() <= NRInternalSize(base).Area() && better(candidate, best)) { best = candidate; }
            }
        }
        Require(Same(SelectNRFriendlyRenderSize(base, out, range, {true,true,true,int(seed % 5)}), best), "Cartesian oracle and tie ordering");
    }
    std::printf("PASS %u SDK table selections; 80 Cartesian oracles; eligibility, range, aspect, area and pixel guards\n", count);
}
