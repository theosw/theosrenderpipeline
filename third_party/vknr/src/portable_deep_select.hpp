// Portable deep-GEMM shape selection. No vendor extension or fragment layout is used.
#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>
namespace nrtest {
inline std::string deep_shape_path(VkInstance instance,VkPhysicalDevice physical,const std::string& path) {
    const auto sep=path.find_last_of("/\\");
    const auto name=path.substr(sep==std::string::npos?0:sep+1);
    if(name!="g_gemmprojw.spv" && name!="g_gemmvact.spv")return path;
    const std::string variant=path.substr(0,path.size()-4)+"_deep_m4n2.spv";
    if(!std::ifstream(variant,std::ios::binary))return path; // RDNA3 and untuned packages.
    VkPhysicalDeviceSubgroupSizeControlProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&sg;
    vkGetPhysicalDeviceProperties2(physical,&properties);
    // Conservative default for variable subgroup hardware (AMD wave32/64 and Intel 8/16/32).
    // This shape changes only per-subgroup register ownership, never the 64x128 projection / 64x256 expansion output tiles.
    if(sg.minSubgroupSize!=32 || sg.maxSubgroupSize!=32 ||
       !(sg.requiredSubgroupSizeStages&VK_SHADER_STAGE_COMPUTE_BIT) ||
       properties.properties.limits.maxComputeWorkGroupInvocations<(name=="g_gemmvact.spv"?256u:128u) ||
       properties.properties.limits.maxComputeSharedMemorySize<12288)return path;
    const auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
        vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
    if(!query)return path;
    uint32_t count=0;
    if(query(physical,&count,nullptr)!=VK_SUCCESS)return path;
    std::vector<VkCooperativeMatrixPropertiesKHR> tuples(count,{VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
    if(query(physical,&count,tuples.data())!=VK_SUCCESS)return path;
    for(const auto& t:tuples)if(t.MSize==16 && t.NSize==16 && t.KSize==16 && t.scope==VK_SCOPE_SUBGROUP_KHR &&
       t.AType==VK_COMPONENT_TYPE_FLOAT16_KHR && t.BType==VK_COMPONENT_TYPE_FLOAT16_KHR &&
       t.CType==VK_COMPONENT_TYPE_FLOAT32_KHR && t.ResultType==VK_COMPONENT_TYPE_FLOAT32_KHR && !t.saturatingAccumulation){
        std::fprintf(stderr,"deep: selected queried subgroup32 16x16x16 GEMM shape 4x2: %s\n",variant.c_str());
        return variant;
    }
    return path;
}
}
