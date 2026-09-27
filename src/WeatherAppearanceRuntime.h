#pragma once

#include "WeatherAppearanceController.h"
#include <chrono>
#include <mutex>
#include <memory>
#include <unordered_map>

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
    void Configure(Settings settings) { std::scoped_lock lock(mutex_); controller_.Configure(std::move(settings)); nextIdleSample_ = {}; }
    void Pause(bool paused) { std::scoped_lock lock(mutex_); controller_.Pause(paused); nextIdleSample_ = {}; }
    void Invalidate() { std::scoped_lock lock(mutex_); controller_.Invalidate(); incomingID_ = outgoingID_ = ~0u; nextIdleSample_ = {}; }
    void CaptureCatalogue();
    std::shared_ptr<const std::vector<WeatherEntry>> Catalogue() const { std::scoped_lock lock(mutex_); return catalogue_; }
    float Apply(SourceDLSSG::NeuralOptions& options, float sharpness);
private:
    mutable std::mutex mutex_;
    Controller controller_;
    Context context_;
    std::uint32_t incomingID_{~0u}, outgoingID_{~0u};
    std::shared_ptr<const std::vector<WeatherEntry>> catalogue_{std::make_shared<const std::vector<WeatherEntry>>()};
    std::unordered_map<std::uint32_t, std::size_t> catalogueIndex_;
    void ReadContext();
    std::chrono::steady_clock::time_point lastFrame_{};
    std::chrono::steady_clock::time_point nextIdleSample_{};
};
}
