#pragma once

#include "WeatherAppearance.h"
#include "FrameGen/SourceDLSSGNeuralState.h"

namespace TheosRenderPipeline::Appearance
{
inline Values FromNeural(const SourceDLSSG::NeuralOptions& options, float sharpness)
{
    const auto first = options.tuning;
    const auto second = options.EffectiveSecond().tuning;
    Values values;
    values.passes[0] = {first.intensity, first.localToneStrength, first.localStructureStrength};
    values.passes[1] = {second.intensity, second.localToneStrength, second.localStructureStrength};
    values.sharpness = sharpness;
    return Sanitize(values);
}
inline void ApplyValues(SourceDLSSG::NeuralOptions& options, const Values& values)
{
    auto apply = [](NeuralRendering::Tuning& tuning, const Neural& values) {
        tuning.intensity = values.intensity;
        tuning.localToneStrength = values.tone;
        tuning.localStructureStrength = values.structure;
    };
    apply(options.tuning, values.passes[0]);
    if (!options.secondPass.linked) { apply(options.secondPass.tuning, values.passes[1]); }
}
struct Snapshot
{
    Context context;
    Evaluation result;
    bool paused{};
};

// The production frame decision is independent of engine pointers and GPU work.
// Only frame-local options are modified; base preferences never receive a blend.
class Controller
{
public:
    const Settings& Configuration() const { return settings_; }
    const Snapshot& State() const { return snapshot_; }
    void Configure(Settings settings)
    {
        settings = Sanitize(std::move(settings));
        if (settings_ == settings) { return; }
        settings_ = std::move(settings);
        ++revision_;
        smoother_.Reset();
    }
    void Pause(bool paused)
    {
        if (paused_ != paused) { paused_ = paused; ++revision_; smoother_.Reset(); }
        snapshot_.paused = paused_;
    }
    void Invalidate()
    {
        if (snapshot_.context.valid || snapshot_.result.active) { ++revision_; }
        snapshot_.context.valid = false;
        snapshot_.result.active = false;
        smoother_.Reset();
    }
    float Apply(Context context, SourceDLSSG::NeuralOptions& options, float sharpness, float elapsed)
    {
        const auto& old = snapshot_.context;
        const auto hourDelta = std::abs(context.hour - old.hour);
        const bool timeJump = (std::min)(hourDelta, 24.0f - hourDelta) > 0.25f;
        const bool forcedWeather = context.incoming.record != old.incoming.record &&
            (context.outgoing.record != old.incoming.record || context.transition >= 1);
        if (options != previousBase_ || sharpness != previousSharpness_ || context.valid != old.valid ||
            context.interior != old.interior || timeJump || forcedWeather) { ++revision_; }
        previousBase_ = options;
        previousSharpness_ = sharpness;
        auto effectiveContext = context;
        effectiveContext.valid &= !paused_;
        auto result = Evaluate(settings_, effectiveContext, FromNeural(options, sharpness));
        result.values = smoother_.Update(result.values, settings_.smoothingSeconds, elapsed, result.active);
        if (options.secondPass.linked) { result.values.passes[1] = result.values.passes[0]; }
        if (result.active) {
            ApplyValues(options, result.values);
            // Ordinary weather/time progress shares a revision. Manual edits,
            // profile edits, loads and discontinuities start a new history.
            options.appearanceRevision = revision_;
        }
        snapshot_ = {std::move(context), result, paused_};
        return result.values.sharpness;
    }
private:
    Settings settings_;
    Snapshot snapshot_;
    Smoother smoother_;
    SourceDLSSG::NeuralOptions previousBase_;
    float previousSharpness_{};
    std::uint64_t revision_{1};
    bool paused_{};
};
}
