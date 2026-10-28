#pragma once
#include <dxgi.h>

namespace TheosRenderPipeline::SourceDLSSG
{
    enum class VSyncMode { Auto, Off, On };
    constexpr bool ValidVSync(int value) { return value >= 0 && value <= 2; }
    constexpr VSyncMode SanitizeVSync(VSyncMode value) { return ValidVSync(static_cast<int>(value)) ? value : VSyncMode::Auto; }
    constexpr const char* VSyncName(VSyncMode value) {
        return value == VSyncMode::Off ? "Off" : value == VSyncMode::On ? "On" : "Auto";
    }
    struct VSyncConfiguration {
        VSyncMode mode{VSyncMode::Auto};
        bool meteringRequested{}, allowTearing{};
        // Streamline's DLSS-G requirements say VSync must be off while it generates.
        bool offRequired{};
    };
    constexpr bool WantsImmediate(VSyncConfiguration c) {
        return c.mode == VSyncMode::Off || (c.mode == VSyncMode::Auto && (c.meteringRequested || c.offRequired));
    }
    struct VSyncSubmission {
        VSyncMode mode{VSyncMode::Auto};
        UINT incomingSync{}, incomingFlags{}, sync{}, flags{};
        bool autoMetering{}, windowed{}, autoRequiredOff{};
        bool operator==(const VSyncSubmission&) const = default;
    };
    constexpr VSyncSubmission ResolveVSync(VSyncConfiguration c, UINT sync, UINT flags, bool windowed) {
        VSyncSubmission out{SanitizeVSync(c.mode), sync, flags, sync, flags, false, windowed};
        // Test Presents are probes, never a settings application or observation.
        if (flags & DXGI_PRESENT_TEST) return out;
        if (out.mode == VSyncMode::On) {
            out.sync = 1; out.flags &= ~UINT(DXGI_PRESENT_ALLOW_TEARING);
        } else if (WantsImmediate({out.mode, c.meteringRequested, c.allowTearing, c.offRequired})) {
            out.sync = 0;
            out.flags &= ~UINT(DXGI_PRESENT_ALLOW_TEARING);
            if (c.allowTearing && windowed) out.flags |= DXGI_PRESENT_ALLOW_TEARING;
            out.autoMetering = out.mode == VSyncMode::Auto && c.meteringRequested;
            out.autoRequiredOff = out.mode == VSyncMode::Auto && !c.meteringRequested && c.offRequired;
        }
        return out;
    }
}
