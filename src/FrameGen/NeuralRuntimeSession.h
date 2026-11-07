#pragma once
#include "NeuralBackend.h"
#include <mutex>

namespace TheosRenderPipeline::NeuralRendering {
inline constexpr const char* kRuntimeRestart = "HIP/ZLUDA and Vulkan cannot run in the same session; restart Skyrim to switch.";
inline constexpr const char* kProbeFinishing = "Vulkan NR capability check is finishing. Try Apply again shortly.";

// Ownership is permanent after admission: both runtimes retain process-wide
// modules and callbacks even after a feature has retired. A temporary Vulkan
// capability probe must also finish before HIP may enter the process.
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
        std::scoped_lock lock(mutex_);
        if (owner_ == Backend::HIP) return false;
        ++probes_;
        return true;
    }
    void EndVulkanProbe() {
        std::scoped_lock lock(mutex_);
        --probes_;
    }
private:
    const char* RefusalLocked(Backend requested) const {
        if (requested == Backend::Native) return nullptr;
        if (owner_ != Backend::Native && owner_ != requested) return kRuntimeRestart;
        if (requested == Backend::HIP && probes_) return kProbeFinishing;
        return nullptr;
    }
    mutable std::mutex mutex_;
    Backend owner_{};
    unsigned probes_{};
};
inline RuntimeSession runtimeSession;
}
