#pragma once
#include "UpscalerInputResolution.h"
#include <cstdint>
#include <array>

namespace TheosRenderPipeline::NeuralRendering
{
    struct InternalSize
    {
        std::uint64_t width{}, height{}, tokens{}, quantumWidth{}, quantumHeight{};
        std::uint64_t Area() const { return width * height; }
    };

    // NR 310.9.16, preset 0 / ratio 1, host sizing RVA 0x5EB80.
    // Count the six encoder decreases and the half-resolution decoder crop.
    inline InternalSize NRInternalSize(FrameExtent input)
    {
        if (!input.width || !input.height) { return {}; }
        const auto down = [](std::uint64_t n) { return 4 * ((n + 7) / 8); };
        const auto quantum = [&](std::uint64_t n) {
            std::uint64_t levels[7]{n}; unsigned decreases = 0;
            for (unsigned i = 1; i != 7; ++i) {
                levels[i] = down(levels[i - 1]);
                decreases += levels[i] < levels[i - 1];
            }
            decreases += 2 * levels[2] > levels[1];
            return std::uint64_t{1} << decreases;
        };
        const auto qw = quantum(input.width), qh = quantum(input.height);
        auto w = (std::max)(std::uint64_t{320}, ((input.width + qw - 1) / qw) * qw);
        const auto h = (std::max)(std::uint64_t{320}, ((input.height + qh - 1) / qh) * qh);
        if (w % (4 * qw) == 0 && h % (4 * qh) == 0) { w += qw; }
        auto tw = w, th = h;
        for (unsigned i = 0; i != 6; ++i) { tw = down(tw); th = down(th); }
        return {w, h, tw * th, qw, qh};
    }

    struct RenderSizePolicy
    {
        bool enabled{}, pascalResearch{}, neuralBeforeUpscaling{};
        int quality{5}; // 0..4 scaled presets; Native/DLAA (5) is excluded.
        bool Active() const { return enabled && pascalResearch && neuralBeforeUpscaling && quality >= 0 && quality < 5; }
    };

    // SDK-independent, exhaustive selection within the admitted range and TRP's
    // existing aspect tolerance. Keep nominal exactly unless NR area strictly falls.
    // This unqualified optimum is for policy evaluation; allocation uses the
    // quality-qualified SelectNRFriendlyRenderSize API below.
    inline FrameExtent SelectNRRenderSizeCandidate(FrameExtent nominal, FrameExtent output,
        const UpscalerInputResolution& range, RenderSizePolicy policy)
    {
        if (!policy.Active() || !range.Accepts(nominal, output) ||
            output.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || output.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) { return nominal; }
        const auto nominalArea = std::uint64_t(nominal.width) * nominal.height;
        const auto internalLimit = NRInternalSize(nominal).Area();
        auto best = nominal;
        auto bestInternal = internalLimit, bestPixels = nominalArea;
        const auto error = [&](FrameExtent size) {
            const auto x = std::uint64_t(size.width) * output.height, y = std::uint64_t(size.height) * output.width;
            return x > y ? x - y : y - x;
        };
        const auto change = [&](FrameExtent size) {
            return (size.width > nominal.width ? size.width - nominal.width : nominal.width - size.width) +
                (size.height > nominal.height ? size.height - nominal.height : nominal.height - size.height);
        };
        const auto tolerance = (std::max)(output.width, output.height);
        const auto maxWidth = (std::min)(range.maximum.width, output.width);
        for (auto w = range.minimum.width; w <= maxWidth; ++w) {
            const auto x = std::uint64_t(w) * output.height;
            const auto low = x > tolerance ? (x - tolerance + output.width - 1) / output.width : 1;
            const auto high = (x + tolerance) / output.width;
            const auto minHeight = (std::max)(std::uint64_t(range.minimum.height), low);
            const auto maxHeight = (std::min)({std::uint64_t(range.maximum.height), std::uint64_t(output.height), high});
            for (auto h = minHeight; h <= maxHeight; ++h) {
                const FrameExtent candidate{w, static_cast<UINT>(h)};
                if (!range.Accepts(candidate, output)) { continue; }
                const auto pixels = std::uint64_t(w) * h;
                if (pixels * 100 < nominalArea * 97) { continue; }
                const auto internal = NRInternalSize(candidate).Area();
                if (internal >= internalLimit) { continue; }
                const auto aspect = error(candidate) * best.height;
                const auto bestAspect = error(best) * h;
                if (internal < bestInternal || (internal == bestInternal &&
                    (pixels > bestPixels || (pixels == bestPixels &&
                        (aspect < bestAspect || (aspect == bestAspect && change(candidate) < change(best))))))) {
                    best = candidate; bestInternal = internal; bestPixels = pixels;
                }
            }
        }
        return best;
    }

    struct QualifiedRenderSizeMove { FrameExtent nominal, selected; };
    // c28-launch, NR 310.9.16 preset 0 / ratio 1, RGB MSE pooled over
    // frames 10..49 against each endpoint's own same-size NVIDIA reference.
    // Adding a pair requires <= 0.5 dB pooled loss relative to its nominal.
    inline constexpr std::array kQualifiedRenderSizeMoves{
        QualifiedRenderSizeMove{{753,424}, {768,432}}, // 50.433040 -> 53.413151 dB
        QualifiedRenderSizeMove{{696,392}, {704,397}}, // 51.540393 -> 51.732637 dB
        QualifiedRenderSizeMove{{742,418}, {768,432}}, // 51.549547 -> 53.413151 dB
    };

    inline bool IsQualityQualifiedMove(FrameExtent nominal, FrameExtent selected)
    {
        for (const auto& move : kQualifiedRenderSizeMoves) {
            if (nominal.width == move.nominal.width && nominal.height == move.nominal.height &&
                selected.width == move.selected.width && selected.height == move.selected.height) { return true; }
        }
        return false;
    }

    enum class RenderSizeDecision { KeptNoSaving, KeptNotQualified, Moved };
    struct RenderSizeSelection { FrameExtent extent; RenderSizeDecision decision; };

    // Qualify the strict-area optimum; never substitute another candidate or
    // infer approval from a nearby size or a shared selected extent.
    inline RenderSizeSelection SelectNRFriendlyRenderSizeWithDecision(FrameExtent nominal, FrameExtent output,
        const UpscalerInputResolution& range, RenderSizePolicy policy)
    {
        const auto candidate = SelectNRRenderSizeCandidate(nominal, output, range, policy);
        if (candidate.width == nominal.width && candidate.height == nominal.height) {
            return {nominal, RenderSizeDecision::KeptNoSaving};
        }
        if (!IsQualityQualifiedMove(nominal, candidate)) { return {nominal, RenderSizeDecision::KeptNotQualified}; }
        return {candidate, RenderSizeDecision::Moved};
    }

    inline FrameExtent SelectNRFriendlyRenderSize(FrameExtent nominal, FrameExtent output,
        const UpscalerInputResolution& range, RenderSizePolicy policy)
    {
        return SelectNRFriendlyRenderSizeWithDecision(nominal, output, range, policy).extent;
    }

}
