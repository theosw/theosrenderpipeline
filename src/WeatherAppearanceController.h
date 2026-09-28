#pragma once

#include "WeatherAppearance.h"
#include "FrameGen/SourceDLSSGNeuralState.h"

namespace TheosRenderPipeline::Appearance
{
inline Setup FromNeural(const SourceDLSSG::NeuralOptions& options, bool sharpening, float sharpness)
{
    Setup setup;
    setup.neural = options.enabled;
    setup.beforeUpscaling = options.beforeUpscaling;
    setup.passes = options.passes;
    setup.combat = options.combat;
    setup.reconstruction = options.reconstruction;
    setup.tuning = options.tuning;
    setup.second = options.secondPass;
    setup.sharpening = sharpening;
    setup.sharpness = sharpness;
    return SanitizeSetup(setup);
}
inline void ApplySetup(SourceDLSSG::NeuralOptions& options, const Setup& setup)
{
    options.enabled = setup.neural;
    options.beforeUpscaling = setup.beforeUpscaling;
    options.passes = setup.passes;
    options.combat = setup.combat;
    // The adapter-owned input contract is never a preset setting.
    const bool producerColor = options.reconstruction.producerColor;
    options.reconstruction = setup.reconstruction;
    options.reconstruction.producerColor = producerColor;
    options.tuning = setup.tuning;
    options.secondPass = setup.second;
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
        twoPassAllocation_ = PresetsRequestTwoPasses(settings_);
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
    void Apply(const Context& context, SourceDLSSG::NeuralOptions& options, bool& sharpening, float& sharpness, float elapsed)
    {
        const auto& old = snapshot_.context;
        const auto hourDelta = std::abs(context.hour - old.hour);
        const bool timeJump = (std::min)(hourDelta, 24.0f - hourDelta) > 0.25f;
        const bool forcedWeather = context.incoming.record != old.incoming.record &&
            (context.outgoing.record != old.incoming.record || context.transition >= 1);
        if (options != previousBase_ || sharpening != previousSharpening_ || sharpness != previousSharpness_ ||
            context.valid != old.valid || context.interior != old.interior || timeJump || forcedWeather) { ++revision_; }
        previousBase_ = options;
        previousSharpening_ = sharpening;
        previousSharpness_ = sharpness;
        auto& result = snapshot_.result;
        result.active = settings_.enabled && context.valid && !paused_ && std::isfinite(context.hour);
        const auto base = FromNeural(options, sharpening, sharpness);
        if (result.active) {
            if (selectionDirty_ || !old.valid || context.interior != old.interior ||
                context.incoming.record != old.incoming.record || context.outgoing.record != old.outgoing.record ||
                context.incoming.group != old.incoming.group || context.outgoing.group != old.outgoing.group) {
                const auto incoming = Select(settings_, context.incoming, context.interior);
                const auto outgoing = Valid(context.outgoing.record) && !context.interior ? Select(settings_, context.outgoing, false) : incoming;
                incoming_ = Flatten(incoming); outgoing_ = Flatten(outgoing);
                result.incoming = Names(incoming); result.outgoing = Names(outgoing);
                result.sharpnessSource = SourceOf(incoming, "Sharpness");
                selectionDirty_ = false;
                ++selectionResolutions_;
            }
            result.setup = Blend(Resolve(outgoing_, settings_, context.hour, base), Resolve(incoming_, settings_, context.hour, base),
                context.interior ? 1 : context.transition);
        } else {
            result.setup = base;
            result.incoming = result.outgoing = result.sharpnessSource = "Base";
            selectionDirty_ = true;
        }
        result.setup = smoother_.Update(result.setup, settings_.smoothingSeconds, elapsed, result.active);
        if (result.active) {
            ApplySetup(options, result.setup);
            // One pass inside a two-pass allocation skips Pass 2 like the combat
            // option, instead of retiring and recreating NR.
            options.presetOnePass = result.setup.passes == 1 && (base.passes == 2 || twoPassAllocation_);
            if (options.presetOnePass) { options.passes = 2; }
            sharpening = result.setup.sharpening;
            sharpness = result.setup.sharpness;
            // Ordinary weather/time progress shares a revision. Manual edits,
            // profile edits, loads and discontinuities start a new history.
            options.appearanceRevision = revision_;
        }
        snapshot_.context = context;
        snapshot_.paused = paused_;
    }
    std::uint64_t SelectionResolutions() const { return selectionResolutions_; }
private:
    // Point into settings_; rebuilt whenever it or the weather changes.
    Changes incoming_, outgoing_;
    bool selectionDirty_{true};
    bool twoPassAllocation_{};
    std::uint64_t selectionResolutions_{};
    Settings settings_;
    Snapshot snapshot_;
    Smoother smoother_;
    SourceDLSSG::NeuralOptions previousBase_;
    bool previousSharpening_{true};
    float previousSharpness_{};
    std::uint64_t revision_{1};
    bool paused_{};
};
}
