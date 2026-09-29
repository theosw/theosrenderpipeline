#include "FrameGen/SourceDLSSGSettings.h"
#include <SimpleIni.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::HDROutput;

static void Require(bool value, const char* why)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

int main()
{
    // Published BT.2100 PQ reference points.
    Require(Near(EncodePQ(0.0f), 0.0f, 1e-6f), "PQ black (c1^m2, about 7e-7)");
    Require(Near(EncodePQ(10000.0f), 1.0f, 1e-6f), "PQ 10000 nits");
    Require(Near(EncodePQ(100.0f), 0.5081f, 5e-4f), "PQ 100 nits");
    Require(Near(EncodePQ(1000.0f), 0.7518f, 5e-4f), "PQ 1000 nits");
    Require(EncodePQ(-5.0f) == EncodePQ(0.0f) && Near(EncodePQ(20000.0f), 1.0f, 1e-6f), "PQ clamps");

    Require(Decode(0.0f, Transfer::Gamma22) == 0.0f && Near(Decode(1.0f, Transfer::Gamma22), 1.0f, 1e-6f), "2.2 endpoints");
    Require(Near(Decode(0.5f, Transfer::Gamma22), 0.21764f, 1e-4f), "2.2 mid grey");
    Require(Near(Decode(0.5f, Transfer::SRGB), 0.21404f, 1e-4f), "sRGB mid grey");
    Require(Near(Decode(0.02f, Transfer::SRGB), 0.02f / 12.92f, 1e-7f), "sRGB linear toe");

    // Expansion: identity below the start point, reaches the maximum at white,
    // monotonic in luminance, preserves chromaticity, never darkens or exceeds.
    constexpr float start = 0.7f, maximum = 5.0f;
    for (float y : {0.0f, 0.1f, 0.5f, 0.69f, 0.7f}) {
        const auto out = Expand({y, y, y}, start, maximum);
        Require(out[0] == y && out[1] == y && out[2] == y, "identity at or below expansion start");
    }
    const auto white = Expand({1, 1, 1}, start, maximum);
    Require(Near(white[0], maximum, 1e-5f) && Near(white[2], maximum, 1e-5f), "white reaches maximum scale");
    float previous = 0.0f;
    for (int i = 0; i <= 1000; ++i) {
        const float y = i / 1000.0f;
        const float value = Expand({y, y, y}, start, maximum)[0];
        Require(value >= previous && value >= y, "grey ramp is monotonic and never darkens");
        previous = value;
    }
    // Slope is continuous at the start (smoothstep has zero derivative there).
    const float e = 1e-4f;
    const float slopeBelow = (Expand({start, start, start}, start, maximum)[0] - Expand({start - e, start - e, start - e}, start, maximum)[0]) / e;
    const float slopeAbove = (Expand({start + e, start + e, start + e}, start, maximum)[0] - Expand({start, start, start}, start, maximum)[0]) / e;
    Require(Near(slopeBelow, 1.0f, 1e-2f) && Near(slopeAbove, 1.0f, 1e-2f), "C1 at expansion start");
    for (RGB colour : {RGB{1.0f, 0.9f, 0.8f}, RGB{0.2f, 0.9f, 1.0f}, RGB{1.0f, 0.0f, 0.0f}, RGB{0.0f, 0.0f, 1.0f}}) {
        const auto out = Expand(colour, start, maximum);
        const float scale = colour[0] > 0 ? out[0] / colour[0] : out[2] / colour[2];
        Require(scale >= 1.0f, "saturated colour never darkens");
        for (int c = 0; c < 3; ++c) {
            Require(out[c] <= maximum + 1e-5f, "no channel exceeds the maximum scale");
            Require(Near(out[c], colour[c] * scale, 1e-5f), "chromaticity preserved");
        }
    }
    const auto dim = Expand({0.0f, 0.0f, 1.0f}, start, maximum);
    Require(dim[2] == 1.0f, "pure blue (low luminance) is not boosted");
    const auto off = Expand({1, 1, 1}, start, 1.0f);
    Require(off[0] == 1.0f, "maximum scale 1 disables expansion");

    // Settings: sanitize, derived maximum and constants.
    Settings s;
    Require(!s.enabled && MaximumScale(s) == 5.0f, "defaults: off, 1000/200 nits");
    s.highlightStrength = 0.0f;
    Require(MaximumScale(s) == 1.0f, "zero strength keeps SDR range");
    s.highlightStrength = 0.5f;
    Require(Near(MaximumScale(s), 3.0f, 1e-6f), "strength blends the maximum");
    Settings bad;
    bad.paperWhiteNits = std::numeric_limits<float>::quiet_NaN();
    bad.peakNits = 10.0f; bad.uiNits = 5000.0f; bad.highlightStrength = 3.0f; bad.expansionStart = 1.5f;
    bad.transfer = static_cast<Transfer>(7);
    bad = Sanitize(bad);
    Require(bad.paperWhiteNits == 200.0f && bad.peakNits == 200.0f && bad.uiNits == 1000.0f &&
        bad.highlightStrength == 1.0f && bad.expansionStart == 0.95f && bad.transfer == Transfer::Gamma22, "sanitize");
    const auto constants = MakeShaderConstants(Settings{}, true);
    Require(constants.scale[0] == 200.0f && constants.scale[1] == 200.0f && constants.scale[3] == 5.0f &&
        constants.mode[1] == 1.0f && Near(constants.red[0], 0.6274f, 1e-4f), "shader constants");

    // Encoded values: SDR white lands at paper white; UI uses UI brightness.
    Settings calibrated; calibrated.highlightStrength = 0.0f; calibrated.uiNits = 100.0f;
    Require(Near(EncodeScene({1, 1, 1}, calibrated)[1], EncodePQ(200.0f), 1e-4f), "scene white at paper white");
    Require(Near(EncodeUI({1, 1, 1}, calibrated)[0], EncodePQ(100.0f), 1e-4f), "UI white at UI brightness");
    Require(Near(EncodeScene({1, 1, 1}, Settings{})[1], EncodePQ(1000.0f), 1e-4f), "expanded white at peak");

    // INI round trip through the shared preferences.
    CSimpleIniA ini;
    Require(ini.LoadData("[SourceDLSSG]\nNRPasses=1\n") >= 0, "old INI");
    auto preferences = SourceDLSSG::LoadPreferences(ini);
    Require(preferences.hdrOutput == Settings{}, "old INI keeps HDR output off with defaults");
    preferences.hdrOutput = {true, 250.0f, 1400.0f, 180.0f, 0.8f, 0.65f, Transfer::SRGB};
    SourceDLSSG::StorePreferences(ini, preferences);
    Require(SourceDLSSG::LoadPreferences(ini) == preferences, "HDR output settings round trip");
    Require(ini.GetBoolValue("HDROutput", "Enabled", false) && ini.GetLongValue("HDROutput", "SDRTransfer", 0) == 1, "HDR INI keys");

    std::printf("HDR output reference checks passed\n");
    return 0;
}
