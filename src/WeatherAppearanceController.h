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
    Controller() = default;
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    const Settings& Configuration() const { return settings_; }
    const Snapshot& State() const { return snapshot_; }
    void Configure(Settings settings)
    {
        settings = Sanitize(std::move(settings));
        if (settings_ == settings) { return; }
        settings_ = std::move(settings);
        selectionDirty_ = true;
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
    float Apply(const Context& context, SourceDLSSG::NeuralOptions& options, float sharpness, float elapsed)
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
        auto& result = snapshot_.result;
        result.active = settings_.enabled && context.valid && !paused_ && std::isfinite(context.hour);
        const auto base = FromNeural(options, sharpness);
        if (result.active) {
            if (selectionDirty_ || !old.valid || context.interior != old.interior ||
                context.incoming.record != old.incoming.record || context.outgoing.record != old.outgoing.record ||
                context.incoming.group != old.incoming.group || context.outgoing.group != old.outgoing.group) {
                incoming_ = Select(settings_, context.incoming, context.interior);
                outgoing_ = Valid(context.outgoing.record) && !context.interior ? Select(settings_, context.outgoing, false) : incoming_;
                result.incoming = incoming_.neuralName; result.outgoing = outgoing_.neuralName;
                result.incomingSharpening = incoming_.sharpeningName; result.outgoingSharpening = outgoing_.sharpeningName;
                selectionDirty_ = false;
                ++selectionResolutions_;
            }
            result.values = Blend(Sample(outgoing_, settings_, context.hour, base), Sample(incoming_, settings_, context.hour, base),
                context.interior ? 1 : context.transition);
        } else {
            result.values = base;
            result.incoming = result.outgoing = result.incomingSharpening = result.outgoingSharpening = "Manual defaults";
            selectionDirty_ = true;
        }
        result.values = smoother_.Update(result.values, settings_.smoothingSeconds, elapsed, result.active);
        if (options.secondPass.linked) { result.values.passes[1] = result.values.passes[0]; }
        if (result.active) {
            ApplyValues(options, result.values);
            // Ordinary weather/time progress shares a revision. Manual edits,
            // profile edits, loads and discontinuities start a new history.
            options.appearanceRevision = revision_;
        }
        snapshot_.context = context;
        snapshot_.paused = paused_;
        return result.values.sharpness;
    }
    std::uint64_t SelectionResolutions() const { return selectionResolutions_; }
private:
    Selection incoming_, outgoing_;
    bool selectionDirty_{true};
    std::uint64_t selectionResolutions_{};
    Settings settings_;
    Snapshot snapshot_;
    Smoother smoother_;
    SourceDLSSG::NeuralOptions previousBase_;
    float previousSharpness_{};
    std::uint64_t revision_{1};
    bool paused_{};
};
}
