#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace TheosRenderPipeline
{
    // Experimental ceiling: five interpolated frames plus the rendered frame.
    inline constexpr std::uint32_t XeFGMaxGeneratedFrames = 5;
    // Intel's scene-change sensitivity: 0 least, 1 most; the SDK default is 0.7.
    // A detected change presents the rendered frame without interpolating.
    inline constexpr float XeFGDefaultSceneChangeThreshold = 0.7f;
    // Hundredths, so a saved value reads back equal and Apply shows no change.
    inline float SanitizeXeFGSceneChange(float value)
    {
        if (!std::isfinite(value)) { return XeFGDefaultSceneChangeThreshold; }
        return std::round(std::clamp(value, 0.0f, 1.0f) * 100.0f) / 100.0f;
    }
    struct XeFGOptions {
        bool experimentalMFG{};
        std::uint32_t generatedFrames{1};
        bool operator==(const XeFGOptions&) const = default;
    };
    inline XeFGOptions SanitizeXeFG(XeFGOptions value)
    {
        value.generatedFrames = std::clamp(value.generatedFrames, 1u, XeFGMaxGeneratedFrames);
        return value;
    }
    inline std::uint32_t XeFGCount(XeFGOptions value, std::uint32_t capacity)
    {
        return value.experimentalMFG
            ? std::clamp(value.generatedFrames, 1u, std::clamp(capacity, 1u, XeFGMaxGeneratedFrames))
            : 1u;
    }
    // The unlock stays installed for the process once applied, and a presenter
    // that tried it serves x2-x6 live: x2 bursts pass the pacing hooks untouched.
    // Only a presenter created without trying it is rebuilt for x3-x6; a refused
    // unlock keeps official x2 until restart rather than retrying every frame.
    inline bool XeFGNeedsRecreation(bool unlockTried, XeFGOptions requested)
    {
        return requested.experimentalMFG && !unlockTried;
    }
}
