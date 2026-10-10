#pragma once
#include <algorithm>
#include <cstdint>
namespace TheosRenderPipeline
{
    // Experimental ceiling: five interpolated frames plus the rendered frame.
    inline constexpr std::uint32_t XeFGMaxGeneratedFrames = 5;
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
    inline std::uint32_t XeFGOutputInterval(int fps)
    {
        return fps > 0 ? static_cast<std::uint32_t>((1000000u + fps / 2) / fps) : 0u;
    }
}
