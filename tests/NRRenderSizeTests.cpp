#include "FrameGen/NRRenderSize.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <tuple>

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
        Require(Same(chosen, expected), "independent strict-area and exact-pair qualification oracle");
        Require(range.Accepts(chosen, output), "SDK range and existing aspect check");
        Require(Same(chosen, nominal) || NRInternalSize(chosen).Area() < NRInternalSize(nominal).Area(), "every move strictly reduces NR internal area");
        Require(std::uint64_t(chosen.width) * chosen.height * 100 >= std::uint64_t(nominal.width) * nominal.height * 97, "97 percent nominal pixels");
        for (auto off : {RenderSizePolicy{false, true, true, quality}, RenderSizePolicy{true, false, true, quality},
            RenderSizePolicy{true, true, false, quality}, RenderSizePolicy{true, true, true, 5}}) {
            Require(Same(SelectNRFriendlyRenderSize(nominal, output, range, off), nominal), "switch off, RTX/nonresearch, non-NR/after-upscale, Native unchanged");
        }
        ++count;
    }
    const FrameExtent nominal{753, 424}, output{1280, 720};
    for (const bool enabled : {false, true}) {
        struct Options { bool enabled, beforeUpscaling; } nr{enabled, true};
        const auto startup = StartupRenderSizePolicy(true, true, nr, 2);
        const UpscalerInputResolution admitted{nominal, {640,360}, {1280,720}};
        Require(startup.Active() && !startup.InactiveReason() &&
            Same(SelectNRFriendlyRenderSize(nominal, output, admitted, startup), {768,432}),
            "Pascal before-upscaling startup allocates 768x432 with NR both off and on");
        nr.beforeUpscaling = false;
        const auto after = StartupRenderSizePolicy(true, true, nr, 2);
        Require(!after.Active() && std::string(after.InactiveReason()) == "NR placement after upscaling" &&
            Same(SelectNRFriendlyRenderSize(nominal, output, admitted, after), nominal), "after-upscaling placement explains inactive startup sizing");
        Require(std::string(StartupRenderSizePolicy(true, false, nr, 2).InactiveReason()) == "Pascal research inactive", "non-Pascal inactive reason");
        nr.beforeUpscaling = true;
        Require(std::string(StartupRenderSizePolicy(true, true, nr, 5).InactiveReason()) == "native or invalid quality", "native quality inactive reason");
    }
    const RenderSizePolicy on{true, true, true, 2};
    const UpscalerInputResolution fixed{nominal, nominal, nominal};
    Require(Same(SelectNRFriendlyRenderSize(nominal, output, fixed, on), nominal), "range refuses all moves");
    Require(Same(SelectNRFriendlyRenderSize(nominal, output, {}, on), nominal), "unavailable range keeps nominal");
    const UpscalerInputResolution badAspect{{753,425},{753,425},{753,425}};
    Require(!badAspect.Accepts({753,425}, output) && Same(SelectNRFriendlyRenderSize(nominal, output, badAspect, on), nominal), "range cannot authorize aspect refusal");
    Require(NRInternalSize({0, 424}).Area() == 0, "invalid dimension");
    const UpscalerInputResolution fillOnly{{960,540},{960,540},{1920,1080}};
    Require(NRInternalSize({960,541}).Area() == NRInternalSize({960,540}).Area(), "equal-work regression fixture");
    Require(Same(SelectNRFriendlyRenderSize({960,540}, {1920,1080}, fillOnly, on), {960,540}), "keep exact nominal instead of fill-only 960x541");
    const UpscalerInputResolution quality720{nominal, nominal, output};
    Require(Same(SelectNRFriendlyRenderSize(nominal, output, quality720, on), {768,432}), "720p Quality retains strict saving");
    for (const auto [base, selected, out] : {std::tuple{FrameExtent{753,424}, FrameExtent{768,432}, output},
        std::tuple{FrameExtent{696,392}, FrameExtent{704,397}, FrameExtent{1600,900}},
        std::tuple{FrameExtent{742,418}, FrameExtent{768,432}, output}}) {
        const UpscalerInputResolution admitted{base, base, out};
        for (int quality = 0; quality < 5; ++quality) {
            const auto approved = SelectNRFriendlyRenderSizeWithDecision(base, out, admitted, {true,true,true,quality});
            Require(Same(approved.extent, selected) && approved.decision == RenderSizeDecision::Moved, "each exact qualified pair moves");
            Require(Same(SelectNRFriendlyRenderSize(base, out, admitted, {false,true,true,quality}), base), "switch off bypasses qualification");
        }
    }
    const UpscalerInputResolution quality1080{{1280,720},{960,540},{1920,1080}};
    Require(Same(SelectNRRenderSizeCandidate({1280,720}, {1920,1080}, quality1080, on), {1280,721}), "1280x720 strict-area candidate unchanged");
    const auto blocked720 = SelectNRFriendlyRenderSizeWithDecision({1280,720}, {1920,1080}, quality1080, on);
    Require(Same(blocked720.extent, {1280,720}) && blocked720.decision == RenderSizeDecision::KeptNotQualified, "1280x721 is not quality-qualified");
    const UpscalerInputResolution quality900{{942,530},{942,530},{1600,900}};
    Require(Same(SelectNRRenderSizeCandidate({942,530}, {1600,900}, quality900, on), {960,541}), "960x541 strict-area candidate unchanged");
    const auto blocked900 = SelectNRFriendlyRenderSizeWithDecision({942,530}, {1600,900}, quality900, on);
    Require(Same(blocked900.extent, {942,530}) && blocked900.decision == RenderSizeDecision::KeptNotQualified, "960x541 is not quality-qualified");
    Require(SelectNRFriendlyRenderSizeWithDecision({960,540}, {1920,1080}, fillOnly, on).decision == RenderSizeDecision::KeptNoSaving, "fill-only move keeps no-saving reason");
    const UpscalerInputResolution neighbor{{754,424},{754,424},output};
    Require(Same(SelectNRRenderSizeCandidate({754,424}, output, neighbor, on), {768,432}), "neighbor can share the qualified destination");
    Require(Same(SelectNRFriendlyRenderSize({754,424}, output, neighbor, on), {754,424}), "qualification is exact in nominal dimensions");
    Require(!IsQualityQualifiedMove(nominal, {754,425}), "qualification is exact in selected dimensions");
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
                    NRInternalSize(candidate).Area() < NRInternalSize(base).Area() && better(candidate, best)) { best = candidate; }
            }
        }
        Require(Same(SelectNRRenderSizeCandidate(base, out, range, {true,true,true,int(seed % 5)}), best), "Cartesian oracle and tie ordering");
        Require(Same(SelectNRFriendlyRenderSize(base, out, range, {true,true,true,int(seed % 5)}), base), "unqualified Cartesian optimum keeps exact nominal");
    }
    std::printf("PASS %u SDK table selections; 80 Cartesian oracles; eligibility, range, aspect, strict area saving, exact-pair qualification and pixel guards\n", count);
}
