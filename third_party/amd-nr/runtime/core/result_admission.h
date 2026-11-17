#pragma once
#include <cstdint>

namespace dlss5nr {
// A valid motion chain does not make an old inference result current. Bound
// capture-to-arrival age before replacing the last accepted result. This is
// separate from the existing reuse/fade clock, which starts on acceptance.
inline bool result_arrival_usable(std::uint64_t current, std::uint64_t capture, unsigned maxAge) {
    return capture <= current && current - capture <= maxAge;
}
}
