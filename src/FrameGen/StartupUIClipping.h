#pragma once

#include "NativeUIFrame.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace TheosRenderPipeline
{
    // Clip rectangles use the producer's original framebuffer coordinates.
    // Remember only a full native -> render startup viewport substitution;
    // already-render-sized producers and the later native UI route stay separate.
    class StartupUIClipping
    {
    public:
        struct Clips
        {
            UINT count{};
            std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> rects{};
        };
        void Reset() { context_ = nullptr; requested_.count = 0; }

        // A renderer may restore scissors before it restores its viewport.
        // Retain the exact request so the next viewport can undo/reclassify our
        // conversion without trying to invert rounded pixel coordinates.
        Clips EndViewport(const void* context, std::uint64_t frame)
        {
            Clips result;
            if (context_ && context == context_ && frame == frame_) { result = requested_; }
            Reset();
            return result;
        }

        void Observe(const void* context, std::uint64_t frame, ViewportChange change,
            const D3D11_VIEWPORT& input, const D3D11_VIEWPORT& output, UINT nativeWidth, UINT nativeHeight)
        {
            Reset();
            if (!context || change != ViewportChange::StartupRender || !nativeWidth || !nativeHeight ||
                input.Width != float(nativeWidth) || input.Height != float(nativeHeight) ||
                !std::isfinite(input.TopLeftX) || !std::isfinite(input.TopLeftY) ||
                !std::isfinite(output.TopLeftX) || !std::isfinite(output.TopLeftY) ||
                !std::isfinite(output.Width) || !std::isfinite(output.Height) ||
                output.Width <= 0 || output.Height <= 0 ||
                (input.Width == output.Width && input.Height == output.Height)) { return; }
            context_ = context;
            frame_ = frame;
            source_ = input;
            destination_ = output;
        }

        bool Transform(const void* context, std::uint64_t frame, const NativeUIFrame& phase,
            UINT count, const D3D11_RECT* input, D3D11_RECT* output)
        {
            if (!context_ || context != context_ || frame != frame_ || !phase.ReduceStartupViewport(true) ||
                !count || count > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE || !input || !output) { return false; }
            requested_.count = count;
            std::copy_n(input, count, requested_.rects.begin());
            const double sx = double(destination_.Width) / source_.Width;
            const double sy = double(destination_.Height) / source_.Height;
            const auto edge = [](double value, bool upper) {
                const double rounded = upper ? std::ceil(value) : std::floor(value);
                return static_cast<LONG>(std::clamp(rounded,
                    double((std::numeric_limits<LONG>::min)()), double((std::numeric_limits<LONG>::max)())));
            };
            for (UINT i = 0; i < count; ++i) {
                output[i] = {
                    edge(destination_.TopLeftX + (double(input[i].left) - source_.TopLeftX) * sx, false),
                    edge(destination_.TopLeftY + (double(input[i].top) - source_.TopLeftY) * sy, false),
                    edge(destination_.TopLeftX + (double(input[i].right) - source_.TopLeftX) * sx, true),
                    edge(destination_.TopLeftY + (double(input[i].bottom) - source_.TopLeftY) * sy, true)};
                // Outward rounding must not turn an intentionally empty clip into pixels.
                if (input[i].right <= input[i].left) { output[i].right = output[i].left; }
                if (input[i].bottom <= input[i].top) { output[i].bottom = output[i].top; }
            }
            return true;
        }

    private:
        const void* context_{};
        std::uint64_t frame_{};
        D3D11_VIEWPORT source_{}, destination_{};
        Clips requested_;
    };
}
