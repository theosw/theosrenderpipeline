#pragma once
// Native TRP window policy: rendering the first image must not depend on an
// output heartbeat that only rendering can create. Stale images stay hidden.
#include <cstdint>
namespace fw {
struct NativeVisibility { bool render{}, show{}; };
inline NativeVisibility native_visibility(bool candidate, bool has_frames, std::int64_t now,
    std::int64_t producer, std::int64_t renderer, std::int64_t frequency) {
    const auto fresh = [&](std::int64_t stamp, double limit) {
        return stamp > 0 && frequency > 0 && now >= stamp && double(now - stamp) / double(frequency) < limit;
    };
    const bool render = candidate && fresh(producer, 0.5);
    return {render, render && has_frames && fresh(renderer, 1.0)};
}
}
