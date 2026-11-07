#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <vulkan/vulkan.h>
namespace nr::standalone {
// Only valid during one serialized Runtime construction. No weight file exists.
inline const std::map<std::string,std::vector<uint8_t>>* weights=nullptr;
struct Stage {
 uint32_t index; const char* kernel; int block,layer,width,height,channels;
 VkBuffer raw,expanded; VkDeviceSize offset,bytes;
};
// Per-dispatch diagnostics are opt-in; buffers refer only to activation arenas.
inline std::function<void(VkCommandBuffer,const Stage&,bool)> stage;
}
