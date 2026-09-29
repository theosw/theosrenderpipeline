#pragma once

#include "HDRColorimetry.h"
#include <algorithm>
#include <array>
#include <cmath>

// Renderer-owned HDR output for producers that finish in SDR (ENB and the
// plain TRP route). The producer's display-referred image is decoded, its
// highlights are expanded above paper white, and the result is encoded as
// HDR10/PQ. UI is encoded separately at its own brightness. This is inverse
// tone mapping: highlights the producer already clipped cannot be recovered.
namespace TheosRenderPipeline::HDROutput
{
    enum class Transfer : int { Gamma22 = 0, SRGB = 1 };

    struct Settings
    {
        bool enabled{false};             // Startup-owned: selects the native swapchain format.
        float paperWhiteNits{200.0f};    // SDR white in the scene.
        float peakNits{1000.0f};         // Brightest expanded highlight.
        float uiNits{200.0f};            // Native UI, menus and the TRP overlay.
        float highlightStrength{1.0f};   // 0 = SDR range at paper white; 1 = reach peak.
        float expansionStart{0.7f};      // Linear SDR luminance where expansion begins.
        Transfer transfer{Transfer::Gamma22};
        bool operator==(const Settings&) const = default;
    };

    inline constexpr float kMinimumNits = 80.0f;
    inline constexpr float kMaximumNits = 10000.0f;

    inline Settings Sanitize(Settings value)
    {
        auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
        const Settings defaults;
        value.paperWhiteNits = std::clamp(finite(value.paperWhiteNits, defaults.paperWhiteNits), kMinimumNits, 1000.0f);
        value.peakNits = std::clamp(finite(value.peakNits, defaults.peakNits), value.paperWhiteNits, kMaximumNits);
        value.uiNits = std::clamp(finite(value.uiNits, defaults.uiNits), kMinimumNits, 1000.0f);
        value.highlightStrength = std::clamp(finite(value.highlightStrength, defaults.highlightStrength), 0.0f, 1.0f);
        value.expansionStart = std::clamp(finite(value.expansionStart, defaults.expansionStart), 0.1f, 0.95f);
        if (value.transfer != Transfer::Gamma22 && value.transfer != Transfer::SRGB) { value.transfer = Transfer::Gamma22; }
        return value;
    }

    template <class Ini> Settings Load(const Ini& ini)
    {
        constexpr auto section = "HDROutput";
        Settings value;
        value.enabled = ini.GetBoolValue(section, "Enabled", value.enabled);
        value.paperWhiteNits = static_cast<float>(ini.GetDoubleValue(section, "PaperWhiteNits", value.paperWhiteNits));
        value.peakNits = static_cast<float>(ini.GetDoubleValue(section, "PeakNits", value.peakNits));
        value.uiNits = static_cast<float>(ini.GetDoubleValue(section, "UIBrightnessNits", value.uiNits));
        value.highlightStrength = static_cast<float>(ini.GetDoubleValue(section, "HighlightStrength", value.highlightStrength));
        value.expansionStart = static_cast<float>(ini.GetDoubleValue(section, "ExpansionStart", value.expansionStart));
        value.transfer = static_cast<Transfer>(ini.GetLongValue(section, "SDRTransfer", static_cast<long>(value.transfer)));
        return Sanitize(value);
    }

    template <class Ini> void Store(Ini& ini, Settings value)
    {
        value = Sanitize(value);
        constexpr auto section = "HDROutput";
        ini.SetBoolValue(section, "Enabled", value.enabled);
        ini.SetDoubleValue(section, "PaperWhiteNits", value.paperWhiteNits);
        ini.SetDoubleValue(section, "PeakNits", value.peakNits);
        ini.SetDoubleValue(section, "UIBrightnessNits", value.uiNits);
        ini.SetDoubleValue(section, "HighlightStrength", value.highlightStrength);
        ini.SetDoubleValue(section, "ExpansionStart", value.expansionStart);
        ini.SetLongValue(section, "SDRTransfer", static_cast<long>(value.transfer));
    }

    // Maximum scene value relative to paper white.
    inline float MaximumScale(const Settings& s)
    {
        return 1.0f + (s.peakNits / s.paperWhiteNits - 1.0f) * s.highlightStrength;
    }

    // ---- CPU reference. The shader in SourceDLSSGHDROutput.cpp mirrors these. ----
    using RGB = std::array<float, 3>;

    inline float Decode(float encoded, Transfer transfer)
    {
        const float c = std::clamp(encoded, 0.0f, 1.0f);
        if (transfer == Transfer::SRGB) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return std::pow(c, 2.2f);
    }

    inline float Smoothstep(float edge0, float edge1, float x)
    {
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // Linear SDR (paper white = 1) to linear scene relative to paper white.
    // The boost is multiplicative in log space, C1 at the start point, and never
    // darkens. Chromaticity is preserved; no channel exceeds the maximum scale.
    inline RGB Expand(RGB linear, float start, float maximumScale)
    {
        const float y = 0.2126f * linear[0] + 0.7152f * linear[1] + 0.0722f * linear[2];
        const float value = (std::max)({linear[0], linear[1], linear[2]});
        if (maximumScale <= 1.0f || y <= 0.0f || value <= 0.0f) { return linear; }
        float scale = std::pow(maximumScale, Smoothstep(start, 1.0f, y));
        scale = (std::min)(scale, maximumScale / value);
        scale = (std::max)(scale, 1.0f);
        return {linear[0] * scale, linear[1] * scale, linear[2] * scale};
    }

    inline float EncodePQ(float nits)
    {
        constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
        constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f, c3 = 2392.0f / 128.0f;
        const float y = std::clamp(nits / kMaximumNits, 0.0f, 1.0f);
        const float p = std::pow(y, m1);
        return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
    }

    // Linear BT.709 nits to full-range BT.2020 PQ code values (0..1).
    inline RGB EncodeHDR10(RGB nits709)
    {
        const auto& m = SourceDLSSG::HDRColorimetry::k709To2020;
        RGB out{};
        for (unsigned row = 0; row < 3; ++row) {
            const double v = m[row][0] * nits709[0] + m[row][1] * nits709[1] + m[row][2] * nits709[2];
            out[row] = EncodePQ(static_cast<float>((std::max)(v, 0.0)));
        }
        return out;
    }

    inline RGB EncodeScene(RGB encodedSDR, const Settings& s)
    {
        RGB linear{Decode(encodedSDR[0], s.transfer), Decode(encodedSDR[1], s.transfer), Decode(encodedSDR[2], s.transfer)};
        linear = Expand(linear, s.expansionStart, MaximumScale(s));
        return EncodeHDR10({linear[0] * s.paperWhiteNits, linear[1] * s.paperWhiteNits, linear[2] * s.paperWhiteNits});
    }

    inline RGB EncodeUI(RGB encodedSDR, const Settings& s)
    {
        return EncodeHDR10({Decode(encodedSDR[0], s.transfer) * s.uiNits,
            Decode(encodedSDR[1], s.transfer) * s.uiNits, Decode(encodedSDR[2], s.transfer) * s.uiNits});
    }

    // Constants consumed by the output shader: five float4 rows.
    struct ShaderConstants
    {
        std::array<float, 4> red, green, blue; // BT.709 -> BT.2020 rows; w unused.
        std::array<float, 4> scale;            // paper nits, UI nits, expansion start, maximum scale.
        std::array<float, 4> mode;             // transfer, passthrough, 0, 0.
    };
    static_assert(sizeof(ShaderConstants) == 20 * sizeof(float));

    inline ShaderConstants MakeShaderConstants(Settings s, bool passthrough)
    {
        s = Sanitize(s);
        const auto& m = SourceDLSSG::HDRColorimetry::k709To2020;
        ShaderConstants c{};
        c.red = {float(m[0][0]), float(m[0][1]), float(m[0][2]), 0.0f};
        c.green = {float(m[1][0]), float(m[1][1]), float(m[1][2]), 0.0f};
        c.blue = {float(m[2][0]), float(m[2][1]), float(m[2][2]), 0.0f};
        c.scale = {s.paperWhiteNits, s.uiNits, s.expansionStart, MaximumScale(s)};
        c.mode = {static_cast<float>(s.transfer), passthrough ? 1.0f : 0.0f, 0.0f, 0.0f};
        return c;
    }
}
