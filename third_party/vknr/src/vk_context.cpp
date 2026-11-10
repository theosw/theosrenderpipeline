#include "vk_context.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#define NR_DEFINE_VK(name) PFN_##name name=nullptr;
NR_VK_INSTANCE_FUNCTIONS(NR_DEFINE_VK)
NR_VK_DEVICE_FUNCTIONS(NR_DEFINE_VK)
#undef NR_DEFINE_VK

namespace nrtest {
void vkcheck(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(result));
}
std::string json_quote(const std::string& text) {
    std::ostringstream out; out << '"';
    for(unsigned char c:text) {
        if(c=='"'||c=='\\') out << '\\' << c;
        else if(c<32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
std::string hex_bytes(const void* bytes,size_t count) {
    const auto* p=static_cast<const uint8_t*>(bytes); std::ostringstream out;
    for(size_t i=0;i<count;++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(p[i]);
    return out.str();
}
void write_text(const std::filesystem::path& path,const std::string& text) {
    if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path,std::ios::binary); f << text;
    if(!f) throw std::runtime_error("Cannot write report: "+path.u8string());
}
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
    auto& ctx=*static_cast<Context*>(user);
    std::lock_guard<std::mutex> guard(ctx.validation_mutex);
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++ctx.validation_errors;
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++ctx.validation_warnings;
    if(ctx.messages.size()<128) {ctx.messages.emplace_back(data->pMessage?data->pMessage:"");std::fprintf(stderr,"validation: %s\n",data->pMessage?data->pMessage:"");}
    return VK_FALSE;
}
bool Context::has_extension(const char* name) const {
    for(const auto& ext:extensions) if(!std::strcmp(ext.extensionName,name)) return true;
    return false;
}
bool Context::portable_tuple() const {
    for(const auto& t:tuples) if(t.MSize==16&&t.NSize==16&&t.KSize==16&&t.scope==VK_SCOPE_SUBGROUP_KHR
        &&t.AType==VK_COMPONENT_TYPE_FLOAT16_KHR&&t.BType==VK_COMPONENT_TYPE_FLOAT16_KHR
        &&t.CType==VK_COMPONENT_TYPE_FLOAT32_KHR&&t.ResultType==VK_COMPONENT_TYPE_FLOAT32_KHR
        &&!t.saturatingAccumulation) return true;
    return false;
}
Context::Context(bool enable_validation,int device_index,const std::string& luid):validation(enable_validation) {
    try {
    loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!loader) throw std::runtime_error("Cannot load system Vulkan loader");
    wchar_t module_path[32768]{};
    GetModuleFileNameW(loader,module_path,32768);
    loader_path=std::filesystem::path(module_path).u8string();
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
    if(!gipa) throw std::runtime_error("System Vulkan loader lacks vkGetInstanceProcAddr");
    auto create=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    auto layer_list=reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(gipa(nullptr,"vkEnumerateInstanceLayerProperties"));
    if(validation) {
        uint32_t n=0; vkcheck(layer_list(&n,nullptr),"Enumerate layers");
        std::vector<VkLayerProperties> layers(n); vkcheck(layer_list(&n,layers.data()),"Enumerate layers");
        bool found=false; for(const auto& l:layers) found|=!std::strcmp(l.layerName,"VK_LAYER_KHRONOS_validation");
        if(!found) throw std::runtime_error("Validation requested but VK_LAYER_KHRONOS_validation is unavailable; use process-local VK_LAYER_PATH");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="TRP private NR Vulkan runner"; app.apiVersion=VK_API_VERSION_1_3;
    const char* debug_ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char* layer="VK_LAYER_KHRONOS_validation";
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback=debug_callback; debug.pUserData=this;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app;
    if(validation) { ci.enabledLayerCount=1;ci.ppEnabledLayerNames=&layer;ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=&debug_ext;ci.pNext=&debug; }
    vkcheck(create(&ci,nullptr,&instance),"vkCreateInstance");
#define NR_LOAD_INSTANCE(name) name=reinterpret_cast<PFN_##name>(gipa(instance,#name)); if(!name) throw std::runtime_error("Missing " #name);
    NR_VK_INSTANCE_FUNCTIONS(NR_LOAD_INSTANCE)
#undef NR_LOAD_INSTANCE
    if(validation) {
        auto create_debug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(gipa(instance,"vkCreateDebugUtilsMessengerEXT"));
        vkcheck(create_debug(instance,&debug,nullptr,&messenger),"Create debug messenger");
    }
    uint32_t count=0;vkcheck(vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate devices");
    std::vector<VkPhysicalDevice> devices(count);vkcheck(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Enumerate devices");
    for(uint32_t i=0;i<count;++i) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&id;
        vkGetPhysicalDeviceProperties2(devices[i],&p);
        bool choose=device_index>=0?int(i)==device_index:p.properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if(!luid.empty()) choose=id.deviceLUIDValid&&hex_bytes(id.deviceLUID,VK_LUID_SIZE)==luid;
        if(choose) {physical=devices[i];selected_index=i;break;}
    }
    if(!physical) throw std::runtime_error("Requested Vulkan device/LUID was not found");
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext=&identity; identity.pNext=&subgroup;
    subgroup.pNext=&subgroupDefault; subgroupDefault.pNext=&driver;
    vkGetPhysicalDeviceProperties2(physical,&props);properties=props.properties;
    vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    count=0;vkcheck(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr),"Enumerate device extensions");
    extensions.resize(count);vkcheck(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,extensions.data()),"Enumerate device extensions");
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext=&f11;f11.pNext=&f12;f12.pNext=&f13;
    if(has_extension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) {f13.pNext=&coop;
        if(has_extension(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) coop.pNext=&float8;
    } else if(has_extension(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) f13.pNext=&float8;
    vkGetPhysicalDeviceFeatures2(physical,&features);
    if(has_extension(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME)){VkPhysicalDeviceFeatures2 extra{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};extra.pNext=&workgroup_layout;vkGetPhysicalDeviceFeatures2(physical,&extra);}
    if(has_extension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) {
        auto get_coop=reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(gipa(instance,"vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
        if(!get_coop) throw std::runtime_error("Cooperative matrix extension lacks its query function");
        count=0;vkcheck(get_coop(physical,&count,nullptr),"Query cooperative tuples");
        tuples.resize(count,{VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
        vkcheck(get_coop(physical,&count,tuples.data()),"Query cooperative tuples");tuples.resize(count);
    }
    count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    for(uint32_t i=0;i<count;++i) if((families[i].queueFlags&VK_QUEUE_COMPUTE_BIT)&&families[i].timestampValidBits) {family=i;break;}
    } catch(...) {
        // No device/queue exists in this constructor. Release failed capability
        // probes without depending on partially populated global dispatch.
        if(loader&&instance){
            auto get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
            if(messenger){auto fn=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(get(instance,"vkDestroyDebugUtilsMessengerEXT"));if(fn)fn(instance,messenger,nullptr);}
            auto fn=reinterpret_cast<PFN_vkDestroyInstance>(get(instance,"vkDestroyInstance"));if(fn)fn(instance,nullptr);
        }
        if(loader)FreeLibrary(loader);
        throw;
    }
}
Context::~Context() {
    destroy_device();
    if(validation)std::fprintf(stderr,"TRP Vulkan validation: errors=%u warnings=%u\n",unsigned(validation_errors),unsigned(validation_warnings));
    if(messenger) {
        auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
        auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(gipa(instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(instance,messenger,nullptr);
    }
    if(instance)vkDestroyInstance(instance,nullptr);
    if(loader)FreeLibrary(loader);
}
void Context::destroy_device(){if(device){vkDeviceWaitIdle(device);if(command_pool)vkDestroyCommandPool(device,command_pool,nullptr);vkDestroyDevice(device,nullptr);command_pool=VK_NULL_HANDLE;device=VK_NULL_HANDLE;queue=VK_NULL_HANDLE;}}
void Context::create_device() {
    if(!portable_tuple()||!coop.cooperativeMatrix||!f12.shaderFloat16||!f11.storageBuffer16BitAccess
        ||!f12.timelineSemaphore||!f13.subgroupSizeControl||!f13.computeFullSubgroups
        ||subgroup.minSubgroupSize>32||subgroup.maxSubgroupSize<32
        ||!(subgroup.requiredSubgroupSizeStages&VK_SHADER_STAGE_COMPUTE_BIT)||family==UINT32_MAX)
        throw std::runtime_error("Required portable FP16/FP32 16x16x16 subgroup-32 capabilities are missing");
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};e11.storageBuffer16BitAccess=VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};e12.shaderFloat16=VK_TRUE;e12.timelineSemaphore=VK_TRUE;e12.vulkanMemoryModel=VK_TRUE;
    e12.storageBuffer8BitAccess=f12.storageBuffer8BitAccess;e12.shaderInt8=f12.shaderInt8;e12.scalarBlockLayout=f12.scalarBlockLayout;
    e12.shaderSubgroupExtendedTypes=f12.shaderSubgroupExtendedTypes;e12.vulkanMemoryModelDeviceScope=f12.vulkanMemoryModelDeviceScope;
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};e13.subgroupSizeControl=VK_TRUE;e13.computeFullSubgroups=VK_TRUE;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR ec{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};ec.cooperativeMatrix=VK_TRUE;
    e11.pNext=&e12;e12.pNext=&e13;e13.pNext=&ec;
    std::vector<const char*> exts{VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR ew=workgroup_layout;
    if(has_extension(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME)){ec.pNext=&ew;exts.push_back(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);}
    for(const char* ext:{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME})if(has_extension(ext))exts.push_back(ext);
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};ci.pNext=&e11;ci.queueCreateInfoCount=1;ci.pQueueCreateInfos=&qi;ci.enabledExtensionCount=uint32_t(exts.size());ci.ppEnabledExtensionNames=exts.data();
    VkPhysicalDeviceFeatures2 queried{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};vkGetPhysicalDeviceFeatures2(physical,&queried);
    VkPhysicalDeviceFeatures basic{};basic.shaderInt16=queried.features.shaderInt16;basic.shaderInt64=queried.features.shaderInt64;
    basic.shaderStorageImageWriteWithoutFormat=queried.features.shaderStorageImageWriteWithoutFormat;
    basic.shaderStorageImageReadWithoutFormat=queried.features.shaderStorageImageReadWithoutFormat;ci.pEnabledFeatures=&basic;
    vkcheck(vkCreateDevice(physical,&ci,nullptr,&device),"vkCreateDevice");
#define NR_LOAD_DEVICE(name) name=reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device,#name));if(!name)throw std::runtime_error("Missing " #name);
    NR_VK_DEVICE_FUNCTIONS(NR_LOAD_DEVICE)
#undef NR_LOAD_DEVICE
    vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkcheck(vkCreateCommandPool(device,&pi,nullptr,&command_pool),"Create command pool");
}

std::string Context::capabilities_json() const {
    std::lock_guard<std::mutex> guard(validation_mutex);
    std::ostringstream o;o << std::boolalpha;
    o << "{\n\"schema_version\":1,\n\"loader\":" << json_quote(loader_path)
      << ",\n\"device_index\":" << selected_index << ",\n\"device_name\":" << json_quote(properties.deviceName)
      << ",\n\"vendor_id\":" << properties.vendorID << ",\"device_id\":" << properties.deviceID
      << ",\"api_version\":" << properties.apiVersion << ",\"driver_version\":" << properties.driverVersion
      << ",\"driver_id\":" << driver.driverID << ",\"driver_name\":" << json_quote(driver.driverName)
      << ",\"driver_info\":" << json_quote(driver.driverInfo) << ",\"subgroup_default\":" << subgroupDefault.subgroupSize
      << ",\n\"device_luid_valid\":" << bool(identity.deviceLUIDValid) << ",\"device_luid_bytes\":" << json_quote(hex_bytes(identity.deviceLUID,VK_LUID_SIZE))
      << ",\"node_mask\":" << identity.deviceNodeMask << ",\n\"device_uuid\":" << json_quote(hex_bytes(identity.deviceUUID,VK_UUID_SIZE))
      << ",\"driver_uuid\":" << json_quote(hex_bytes(identity.driverUUID,VK_UUID_SIZE))
      << ",\"pipeline_cache_uuid\":" << json_quote(hex_bytes(properties.pipelineCacheUUID,VK_UUID_SIZE));
    bool dxgi_match=false;Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for(UINT i=0;;++i) {Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 d{};if(adapter&&SUCCEEDED(adapter->GetDesc1(&d))&&identity.deviceLUIDValid
                &&!std::memcmp(&d.AdapterLuid,identity.deviceLUID,VK_LUID_SIZE))dxgi_match=true;
        }
    }
    o << ",\n\"d3d12_adapter_luid_match\":" << dxgi_match << ",\"compute_queue_family\":" << family
      << ",\"timestamp_period_ns\":" << properties.limits.timestampPeriod
      << ",\"max_compute_shared_memory_bytes\":" << properties.limits.maxComputeSharedMemorySize
      << ",\"workgroup_explicit_layout\":" << bool(workgroup_layout.workgroupMemoryExplicitLayout)
      << ",\"workgroup_explicit_layout_8bit\":" << bool(workgroup_layout.workgroupMemoryExplicitLayout8BitAccess)
      << ",\"workgroup_explicit_layout_16bit\":" << bool(workgroup_layout.workgroupMemoryExplicitLayout16BitAccess)
      << ",\n\"cooperative_matrix\":" << bool(coop.cooperativeMatrix) << ",\"portable_tuple\":" << portable_tuple()
      << ",\"shader_float16\":" << bool(f12.shaderFloat16) << ",\"storage_buffer_16bit\":" << bool(f11.storageBuffer16BitAccess)
      << ",\"shader_float8_cooperative_matrix\":" << bool(float8.shaderFloat8CooperativeMatrix)
      << ",\"timeline_semaphore\":" << bool(f12.timelineSemaphore)
      << ",\"subgroup_size_control\":" << bool(f13.subgroupSizeControl) << ",\"compute_full_subgroups\":" << bool(f13.computeFullSubgroups)
      << ",\"subgroup_min\":" << subgroup.minSubgroupSize << ",\"subgroup_max\":" << subgroup.maxSubgroupSize
      << ",\"required_subgroup_size_stages\":" << subgroup.requiredSubgroupSizeStages;
    o << ",\n\"external_buffers\":[";
    bool comma=false;
    for(auto type:{VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT}) {
        VkPhysicalDeviceExternalBufferInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;info.handleType=type;
        VkExternalBufferProperties result{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};vkGetPhysicalDeviceExternalBufferProperties(physical,&info,&result);
        if(comma)o << ',';comma=true;o << "{\"handle_type\":" << type << ",\"features\":" << result.externalMemoryProperties.externalMemoryFeatures << ",\"compatible_types\":" << result.externalMemoryProperties.compatibleHandleTypes << '}';
    }
    o << "],\n\"external_images\":[";comma=false;
    for(auto format:{VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_SFLOAT}){
        VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};ext.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};info.pNext=&ext;info.format=format;info.type=VK_IMAGE_TYPE_2D;info.tiling=VK_IMAGE_TILING_OPTIMAL;info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};ip.pNext=&ep;auto status=vkGetPhysicalDeviceImageFormatProperties2(physical,&info,&ip);
        if(comma)o<<',';comma=true;o<<"{\"format\":"<<format<<",\"query_status\":"<<status<<",\"features\":"<<ep.externalMemoryProperties.externalMemoryFeatures<<",\"compatible_types\":"<<ep.externalMemoryProperties.compatibleHandleTypes<<'}';
    }
    VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};timeline.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};si.pNext=&timeline;si.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};vkGetPhysicalDeviceExternalSemaphoreProperties(physical,&si,&sp);
    o << "],\n\"d3d12_fence_features\":" << sp.externalSemaphoreFeatures << ",\"d3d12_fence_compatible_types\":" << sp.compatibleHandleTypes;
    o << ",\n\"cooperative_matrix_tuples\":[";comma=false;
    for(const auto& t:tuples) {if(comma)o << ',';comma=true;o << "{\"m\":" << t.MSize << ",\"n\":" << t.NSize << ",\"k\":" << t.KSize << ",\"a\":" << t.AType << ",\"b\":" << t.BType << ",\"c\":" << t.CType << ",\"result\":" << t.ResultType << ",\"scope\":" << t.scope << ",\"saturating\":" << bool(t.saturatingAccumulation) << '}';}
    o << "],\n\"extensions\":[";comma=false;for(const auto& ext:extensions){if(comma)o<<',';comma=true;o<<"{\"name\":"<<json_quote(ext.extensionName)<<",\"revision\":"<<ext.specVersion<<'}';}
    o << "],\n\"validation_enabled\":" << validation << ",\"validation_errors\":" << validation_errors << ",\"validation_warnings\":" << validation_warnings << ",\"validation_messages\":[";
    comma=false;for(const auto& m:messages){if(comma)o<<',';comma=true;o<<json_quote(m);}o << "]\n}\n";return o.str();
}

VkCommandBuffer Context::begin() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=command_pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
    VkCommandBuffer cmd;vkcheck(vkAllocateCommandBuffers(device,&ai,&cmd),"Allocate command buffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkcheck(vkBeginCommandBuffer(cmd,&bi),"Begin command buffer");return cmd;
}
void Context::submit_and_wait(VkCommandBuffer cmd) {
    vkcheck(vkEndCommandBuffer(cmd),"End command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;vkcheck(vkCreateFence(device,&fi,nullptr,&fence),"Create fence");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;
    vkcheck(vkQueueSubmit(queue,1,&si,fence),"Submit probe");
    vkcheck(vkWaitForFences(device,1,&fence,VK_TRUE,30000000000ULL),"Wait for probe");
    vkDestroyFence(device,fence,nullptr);vkFreeCommandBuffers(device,command_pool,1,&cmd);
}
Buffer::Buffer(Context& c,VkDeviceSize size,bool visible):context(&c),bytes(size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkcheck(vkCreateBuffer(c.device,&bi,nullptr,&handle),"Create buffer");
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(c.device,handle,&req);
    uint32_t type=UINT32_MAX;VkMemoryPropertyFlags want=visible?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for(uint32_t i=0;i<c.memory.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(c.memory.memoryTypes[i].propertyFlags&want)==want){type=i;break;}
    if(type==UINT32_MAX)throw std::runtime_error("No requested buffer memory type");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
    vkcheck(vkAllocateMemory(c.device,&ai,nullptr,&allocation),"Allocate buffer memory");vkcheck(vkBindBufferMemory(c.device,handle,allocation,0),"Bind buffer memory");
    if(visible)vkcheck(vkMapMemory(c.device,allocation,0,size,0,&mapped),"Map buffer");
}
Buffer::~Buffer(){if(mapped)vkUnmapMemory(context->device,allocation);if(handle)vkDestroyBuffer(context->device,handle,nullptr);if(allocation)vkFreeMemory(context->device,allocation,nullptr);}

Compute::Compute(Context& c,const std::filesystem::path& shader,const std::vector<Buffer*>& buffers):context(&c) {
    std::ifstream f(shader,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Missing shader: "+shader.u8string());
    const auto size=f.tellg();if(size<=0||size%4)throw std::runtime_error("Invalid SPIR-V size");
    std::vector<uint32_t> words(size_t(size)/4);f.seekg(0);f.read(reinterpret_cast<char*>(words.data()),size);if(!f||words[0]!=0x07230203)throw std::runtime_error("Invalid SPIR-V file");
    std::vector<VkDescriptorSetLayoutBinding> bindings;for(uint32_t i=0;i<buffers.size();++i)bindings.push_back({i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sci.bindingCount=uint32_t(bindings.size());sci.pBindings=bindings.data();vkcheck(vkCreateDescriptorSetLayout(c.device,&sci,nullptr,&set_layout),"Create set layout");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,128};VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};lci.setLayoutCount=1;lci.pSetLayouts=&set_layout;lci.pushConstantRangeCount=1;lci.pPushConstantRanges=&push;vkcheck(vkCreatePipelineLayout(c.device,&lci,nullptr,&layout),"Create pipeline layout");
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=size_t(size);smi.pCode=words.data();VkShaderModule module;vkcheck(vkCreateShaderModule(c.device,&smi,nullptr,&module),"Create shader module");
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sub{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};sub.requiredSubgroupSize=32;
    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pci.layout=layout;pci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pci.stage.module=module;pci.stage.pName="main";pci.stage.pNext=&sub;pci.stage.flags=VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    const auto result=vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline);vkDestroyShaderModule(c.device,module,nullptr);vkcheck(result,"Create compute pipeline");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,uint32_t(buffers.size())};VkDescriptorPoolCreateInfo poolci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolci.maxSets=1;poolci.poolSizeCount=1;poolci.pPoolSizes=&ps;vkcheck(vkCreateDescriptorPool(c.device,&poolci,nullptr,&pool),"Create descriptor pool");
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sai.descriptorPool=pool;sai.descriptorSetCount=1;sai.pSetLayouts=&set_layout;vkcheck(vkAllocateDescriptorSets(c.device,&sai,&set),"Allocate descriptor set");
    for(uint32_t i=0;i<buffers.size();++i){VkDescriptorBufferInfo info{buffers[i]->handle,0,buffers[i]->bytes};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&info;vkUpdateDescriptorSets(c.device,1,&w,0,nullptr);}
}
Compute::~Compute(){if(pool)vkDestroyDescriptorPool(context->device,pool,nullptr);if(pipeline)vkDestroyPipeline(context->device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(context->device,layout,nullptr);if(set_layout)vkDestroyDescriptorSetLayout(context->device,set_layout,nullptr);}
void Compute::record(VkCommandBuffer cmd,uint32_t x,uint32_t y,uint32_t z,const void* push,uint32_t bytes) {
    if(bytes>128||bytes%4)throw std::runtime_error("Invalid push constant size");
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    if(bytes)vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,bytes,push);
    vkCmdDispatch(cmd,x,y,z);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_HOST_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
}
}
