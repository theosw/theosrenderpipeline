#pragma once

#include "WeatherAppearanceController.h"
#include <chrono>
#include <mutex>

namespace TheosRenderPipeline::Appearance
{
// Called on the existing world-render boundaries. No game pointers escape into
// the settings/UI snapshot; loading and settings publication share this lock.
class Runtime
{
public:
    static Runtime& Get() { static Runtime value; return value; }
    Settings Configuration() const { std::scoped_lock lock(mutex_); return controller_.Configuration(); }
    Snapshot State() const { std::scoped_lock lock(mutex_); return controller_.State(); }
    void Configure(Settings settings) { std::scoped_lock lock(mutex_); controller_.Configure(std::move(settings)); }
    void Pause(bool paused) { std::scoped_lock lock(mutex_); controller_.Pause(paused); }
    void Invalidate() { std::scoped_lock lock(mutex_); controller_.Invalidate(); }
    float Apply(SourceDLSSG::NeuralOptions& options, float sharpness);
private:
    mutable std::mutex mutex_;
    Controller controller_;
    std::chrono::steady_clock::time_point lastFrame_{};
};
}
