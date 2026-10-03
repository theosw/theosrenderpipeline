#include "FrameGen/NeuralRenderingSubrect.h"
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace TheosRenderPipeline::NeuralRendering;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

// Records the writes the vendor parameter object would receive.
struct Parameters
{
    std::vector<std::pair<std::string, unsigned>> writes;
    void Set(const char* name, unsigned value) { writes.emplace_back(name, value); }
};

int main()
{
    const std::pair<const char*, const SubrectKeys*> cases[]{
        { "Color", &Subrect::Color }, { "MVec", &Subrect::MVec }, { "Depth", &Subrect::Depth },
        { "Output", &Subrect::Output }, { "Backbuffer", &Subrect::Backbuffer },
        { "ControlMask", &Subrect::ControlMask }, { "UI", &Subrect::UI }, { "UIAlpha", &Subrect::UIAlpha },
        { "BidirectionalDistortionField", &Subrect::BidirectionalDistortionField },
    };
    for (const auto& [prefix, keys] : cases) {
        Parameters parameters;
        SetSubrect(&parameters, *keys, 1280u, 720u);
        // The names must match the previously formatted names byte for byte.
        const std::pair<std::string, unsigned> expected[]{
            { std::format("DLSSNR.{}SubrectBaseX", prefix), 0u }, { std::format("DLSSNR.{}SubrectBaseY", prefix), 0u },
            { std::format("DLSSNR.{}SubrectWidth", prefix), 1280u }, { std::format("DLSSNR.{}SubrectHeight", prefix), 720u },
        };
        Require(parameters.writes.size() == std::size(expected), "four writes per subrect");
        for (std::size_t i = 0; i < std::size(expected); ++i) {
            if (parameters.writes[i] != expected[i]) {
                std::fprintf(stderr, "FAIL: %s wrote %s=%u, expected %s=%u\n", prefix, parameters.writes[i].first.c_str(),
                    parameters.writes[i].second, expected[i].first.c_str(), expected[i].second);
                return 1;
            }
        }
    }
    Parameters zero;
    SetSubrect(&zero, Subrect::UI, 0u, 0u);
    Require(zero.writes.size() == 4 && zero.writes[2].second == 0 && zero.writes[3].second == 0,
        "absent optional input still writes an empty subrect");
    std::puts("PASS: nine compile-time DLSSNR subrect key sets match the formatted names and write order");
}
