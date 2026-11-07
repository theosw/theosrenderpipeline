#pragma once
#include <cstdint>
#include "VulkanNRSchedule.h"
namespace TheosRenderPipeline::NeuralRendering {
enum class Backend : std::uint32_t { Native, HIP, Vulkan };
inline constexpr const char* kPreparingVulkan = "Preparing Vulkan NR...";
inline constexpr const char* kVendorEditionReason = "HIP/ZLUDA and Vulkan NR need the Universal edition.";
inline constexpr const char* kVendorUpscalerReason = "HIP/ZLUDA and Vulkan NR require TRP XeSS or FSR. Select one in Image and Apply. These backends are unavailable while Community Shaders owns the upscaler.";
inline constexpr const char* kHIPUpscalerReason = "HIP/ZLUDA NR requires TRP XeSS. FSR has not been validated for this backend; select XeSS and Apply.";
inline constexpr const char* kHIPGPUReason = "HIP/ZLUDA NR requires an AMD Radeon GPU.";
inline constexpr const char* kNativeGPUReason = "Native NR requires NVIDIA hardware. Choose HIP or Vulkan in Universal for this GPU.";

constexpr Backend SanitizeBackend(long value) {
    return value == 1 ? Backend::HIP : value == 2 ? Backend::Vulkan : Backend::Native;
}
constexpr bool TestBackend(Backend value) { return value != Backend::Native; }
constexpr const char* BackendName(Backend value) {
    return value == Backend::Vulkan ? "Vulkan" : value == Backend::HIP ? "HIP/ZLUDA" : "Native";
}
}
