#pragma once
#include <vulkan/vulkan.h>
#include <filesystem>
#include <memory>
#include "../include/vknr.h"
namespace vknr {
struct DeviceContext;
struct DeviceDelete { void operator()(DeviceContext*) const; };
using DeviceOwner = std::unique_ptr<DeviceContext,DeviceDelete>;
DeviceOwner make_device(ID3D12Device*, const std::filesystem::path&, bool, Capability&);
VkInstance instance(DeviceContext*);
VkPhysicalDevice physical(DeviceContext*);
VkDevice device(DeviceContext*);
VkQueue queue(DeviceContext*);
std::uint32_t family(DeviceContext*);
}
