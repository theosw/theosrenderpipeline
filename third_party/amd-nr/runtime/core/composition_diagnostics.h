#pragma once

#include <array>
#include <cstdint>
#include <algorithm>

namespace dlss5nr::diagnostics {
enum class CopyReason : unsigned { None, NotRunning, NoResidual, NoFlow, Expired, DetailZero, Count };
inline const char* Name(CopyReason reason) {
    constexpr const char* names[] = {"none", "not-running", "no-residual", "no-flow", "expired", "detail-zero"};
    return names[static_cast<unsigned>(reason)];
}
// Receives the actual Compose dispatch decision. It never controls rendering.
struct Frame {
    bool apply{}, fresh{}, discarded{}, reset{}, guideChange{}, wait{}, tracking{};
    CopyReason reason{CopyReason::None};
    std::uint32_t age{}, latency{};
};
struct Window {
    std::uint32_t frames{}, fresh{}, reuse{}, copy{}, discarded{}, resets{}, guideChanges{};
    std::uint32_t ageMax{}, latencyMax{}, transitions{}, details{}, suppressed{};
    std::array<std::uint32_t, static_cast<unsigned>(CopyReason::Count)> reasons{};
};
struct Emission { bool detail{}, summary{}; Window window{}; };
class CompositionTrace {
public:
    static constexpr unsigned WindowFrames = 120, DetailLimit = 8;
    Emission Observe(const Frame& frame) {
        const unsigned action = frame.apply ? (frame.fresh ? 1 : 2) : 0;
        const bool changed = !seen_ || action != lastAction_ || frame.reason != lastReason_ ||
            frame.wait != lastWait_ || frame.tracking != lastTracking_ || frame.reset || frame.guideChange;
        seen_ = true; lastAction_ = action; lastReason_ = frame.reason;
        lastWait_ = frame.wait; lastTracking_ = frame.tracking;
        ++window_.frames;
        if (frame.apply) { if (frame.fresh) ++window_.fresh; else ++window_.reuse; }
        else { ++window_.copy; ++window_.reasons[static_cast<unsigned>(frame.reason)]; }
        window_.discarded += frame.discarded; window_.resets += frame.reset; window_.guideChanges += frame.guideChange;
        window_.ageMax = std::max(window_.ageMax, frame.age);
        window_.latencyMax = std::max(window_.latencyMax, frame.latency);
        Emission out;
        if (changed) {
            ++window_.transitions;
            out.detail = window_.details < DetailLimit;
            if (out.detail) ++window_.details; else ++window_.suppressed;
        }
        out.summary = window_.frames == WindowFrames;
        out.window = window_;
        if (out.summary) window_ = {};
        return out;
    }
    Window Pending() const { return window_; }
private:
    Window window_{};
    bool seen_{}, lastWait_{}, lastTracking_{};
    unsigned lastAction_{};
    CopyReason lastReason_{CopyReason::None};
};
}
