#pragma once
#include <cstdint>
#include <string_view>
#include "VulkanNRSchedule.h"
namespace TheosRenderPipeline::NeuralRendering {
enum class Backend : std::uint32_t { Native, HIP, Vulkan };
inline constexpr const char* kPreparingVulkan = "Preparing Vulkan NR...";
inline constexpr const char* kVulkanNotChecked = "Not checked yet — select Vulkan to check this GPU";
inline constexpr const char* kVulkanChecking = "Checking Vulkan NR support...";
inline constexpr const char* kVendorEditionReason = "HIP/ZLUDA and Vulkan NR need the Universal edition.";
inline constexpr const char* kVendorUpscalerReason = "Vulkan NR requires TRP XeSS or FSR, or Community Shaders. Select XeSS or FSR in Image and Apply.";
inline constexpr const char* kHIPUpscalerReason = "HIP/ZLUDA NR requires TRP XeSS or Community Shaders. FSR has not been validated for this backend; select XeSS and Apply.";
inline constexpr const char* kHIPGPUReason = "HIP/ZLUDA NR requires an AMD Radeon GPU.";
inline constexpr const char* kNativeRowReason = "Needs an NVIDIA GPU";
inline constexpr const char* kNativeGPUReason = "Native NR requires NVIDIA hardware. Choose HIP or Vulkan in Universal for this GPU.";
inline std::string_view VulkanChoiceRefusal(std::string_view probeReason, bool upscalerReady) {
    if (!probeReason.empty() && probeReason != kVulkanNotChecked) return probeReason;
    return upscalerReady ? std::string_view{} : kVendorUpscalerReason;
}

constexpr Backend SanitizeBackend(long value) {
    return value == 1 ? Backend::HIP : value == 2 ? Backend::Vulkan : Backend::Native;
}
constexpr bool TestBackend(Backend value) { return value != Backend::Native; }
constexpr const char* BackendName(Backend value) {
    return value == Backend::Vulkan ? "Vulkan" : value == Backend::HIP ? "HIP/ZLUDA" : "Native";
}
}
