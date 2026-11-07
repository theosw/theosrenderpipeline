#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <mutex>

#define NR_VK_INSTANCE_FUNCTIONS(X) \
 X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) \
 X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
 X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
 X(vkGetPhysicalDeviceExternalBufferProperties) X(vkGetPhysicalDeviceExternalSemaphoreProperties) \
 X(vkGetPhysicalDeviceImageFormatProperties2) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define NR_VK_DEVICE_FUNCTIONS(X) \
 X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) \
 X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
 X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) \
 X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
 X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) \
 X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus) \
 X(vkDeviceWaitIdle) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
 X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
 X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
 X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
 X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) \
 X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) \
 X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) \
 X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkGetSemaphoreCounterValue)
#define NR_DECLARE_VK(name) extern PFN_##name name;
NR_VK_INSTANCE_FUNCTIONS(NR_DECLARE_VK)
NR_VK_DEVICE_FUNCTIONS(NR_DECLARE_VK)
#undef NR_DECLARE_VK

namespace nrtest {
void vkcheck(VkResult result, const char* operation);
std::string json_quote(const std::string& text);
std::string hex_bytes(const void* bytes, size_t count);
void write_text(const std::filesystem::path& path, const std::string& text);

struct Context {
    HMODULE loader{};
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool command_pool{};
    uint32_t family=UINT32_MAX;
    uint32_t selected_index=UINT32_MAX;
    bool validation=false;
    mutable std::mutex validation_mutex;
    uint32_t validation_errors=0, validation_warnings=0;
    std::vector<std::string> messages;
    std::string loader_path;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT float8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR workgroup_layout{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    VkPhysicalDeviceSubgroupSizeControlProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    std::vector<VkExtensionProperties> extensions;
    std::vector<VkCooperativeMatrixPropertiesKHR> tuples;
    bool has_extension(const char* name) const;
    bool portable_tuple() const;
    Context(bool enable_validation, int device_index=-1, const std::string& luid={});
    ~Context();
    void destroy_device();
    Context(const Context&)=delete;
    Context& operator=(const Context&)=delete;
    void create_device();
    std::string capabilities_json() const;
    VkCommandBuffer begin();
    void submit_and_wait(VkCommandBuffer cmd); // startup/probes/readback only
};

struct Buffer {
    Context* context{};
    VkBuffer handle{};
    VkDeviceMemory allocation{};
    VkDeviceSize bytes=0;
    void* mapped{};
    Buffer(Context& context, VkDeviceSize bytes, bool host_visible=true);
    ~Buffer();
    Buffer(const Buffer&)=delete;
    Buffer& operator=(const Buffer&)=delete;
};

struct Compute {
    Context* context{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    Compute(Context& context, const std::filesystem::path& shader, const std::vector<Buffer*>& buffers);
    ~Compute();
    Compute(const Compute&)=delete;
    Compute& operator=(const Compute&)=delete;
    void record(VkCommandBuffer cmd, uint32_t x=1, uint32_t y=1, uint32_t z=1,
                const void* push=nullptr, uint32_t push_bytes=0);
};
}
