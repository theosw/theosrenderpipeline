#pragma once
#include <vulkan/vulkan.h>
#include <filesystem>
#include <string>
namespace nrtest {
struct EngineRequest {
 std::filesystem::path dll,input,output,root;
 uint32_t width=0,height=0,frames=2,warmup=1,capture=1;
 int shift=0;
 bool handoff=false,layers=false,stage_timestamps=false;
 std::string backend="portable";
};
std::string run_engine(VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t,const EngineRequest&);
}
