#pragma once
#include <algorithm>
#include <cstdint>
namespace TheosRenderPipeline {
struct XeFGOptions {
    bool experimentalMFG{};
    std::uint32_t generatedFrames{1};
    bool operator==(const XeFGOptions&) const = default;
};
inline XeFGOptions SanitizeXeFG(XeFGOptions value) {
    value.generatedFrames=std::clamp(value.generatedFrames,1u,3u); return value;
}
inline std::uint32_t XeFGCount(XeFGOptions value,std::uint32_t capacity) {
    return value.experimentalMFG?std::clamp(value.generatedFrames,1u,std::clamp(capacity,1u,3u)):1u;
}
inline std::uint32_t XeFGOutputInterval(int fps) {
    return fps>0?static_cast<std::uint32_t>((1000000u+fps/2)/fps):0u;
}
}
