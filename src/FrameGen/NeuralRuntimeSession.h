#pragma once
#include "NeuralBackend.h"
#include <mutex>

namespace TheosRenderPipeline::NeuralRendering {
inline constexpr const char* kRuntimeRestart = "HIP/ZLUDA and Vulkan cannot run in the same session; restart Skyrim to switch.";
inline constexpr const char* kHIPStalled = "HIP/ZLUDA stopped responding; restart Skyrim to use it again.";
inline constexpr const char* kVulkanStalled = "Vulkan NR stopped responding; restart Skyrim to use it again.";

// Ownership is permanent after admission: both runtimes retain process-wide
// modules and callbacks even after a feature has retired. Explicit Vulkan
// probing claims the session too: its engine module remains loaded afterwards.
class RuntimeSession final {
public:
    const char* Refusal(Backend requested) const {
        std::scoped_lock lock(mutex_);
        return RefusalLocked(requested);
    }
    const char* Claim(Backend requested) {
        std::scoped_lock lock(mutex_);
        if (const auto* reason = RefusalLocked(requested)) return reason;
        if (requested != Backend::Native) owner_ = requested;
        return nullptr;
    }
    bool BeginVulkanProbe() {
        return Claim(Backend::Vulkan) == nullptr;
    }
    // Only when LoadLibrary failed completely before any Vulkan module loaded.
    void ReleaseUnloadedVulkanProbe() {
        std::scoped_lock lock(mutex_);
        if (owner_ == Backend::Vulkan && !vulkanFailed_) { owner_ = Backend::Native; }
    }
    // A runtime that stopped responding stays refused until the process restarts.
    void Fail(Backend backend, const char* reason) {
        std::scoped_lock lock(mutex_);
        if (backend == Backend::HIP) { hipFailed_ = true; hipFailedReason_ = reason; }
        else if (backend == Backend::Vulkan) { vulkanFailed_ = true; vulkanFailedReason_ = reason; }
    }
private:
    const char* RefusalLocked(Backend requested) const {
        if (requested == Backend::Native) return nullptr;
        if (requested == Backend::HIP && hipFailed_) return hipFailedReason_;
        if (requested == Backend::Vulkan && vulkanFailed_) return vulkanFailedReason_;
        if (owner_ != Backend::Native && owner_ != requested) return kRuntimeRestart;
        return nullptr;
    }
    mutable std::mutex mutex_;
    Backend owner_{};
    bool hipFailed_{}, vulkanFailed_{};
    const char* hipFailedReason_{};
    const char* vulkanFailedReason_{};
};
inline RuntimeSession runtimeSession;
}
