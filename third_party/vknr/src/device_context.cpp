#include "vk_context.hpp"
#include "probes.hpp"
#include "device_context.hpp"
#include <stdexcept>
#include <cstring>
namespace vknr {
struct DeviceContext { std::unique_ptr<nrtest::Context> context; };
void DeviceDelete::operator()(DeviceContext* p) const { delete p; }
DeviceOwner make_device(ID3D12Device* d3d, const std::filesystem::path& root, bool validation, Capability& caps, Diagnostics diagnostics,
    const Capability* admitted) {
    if (!d3d) throw std::runtime_error("Vulkan NR needs the host D3D12 device");
    DeviceOwner result(new DeviceContext);
    const auto luid=d3d->GetAdapterLuid();
    result->context=std::make_unique<nrtest::Context>(validation,-1,nrtest::hex_bytes(&luid,sizeof(luid)));
    auto& c=*result->context;
    strcpy_s(caps.revision,"coord-r005 / TRP ABI 3"); strcpy_s(caps.device,c.properties.deviceName);
    if (diagnostics) diagnostics(c.capabilities_json().c_str());
    if (!c.has_extension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) ||
        !c.has_extension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME))
        throw std::runtime_error("Vulkan driver lacks Win32 shared texture/fence import");
    for (auto format : {VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_SFLOAT}) {
        VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        ext.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext=&ext; info.format=format; info.type=VK_IMAGE_TYPE_2D; info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2}; ip.pNext=&ep;
        if (vkGetPhysicalDeviceImageFormatProperties2(c.physical,&info,&ip)!=VK_SUCCESS ||
            !(ep.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            throw std::runtime_error("Vulkan driver refuses a required D3D12 shared texture format");
    }
    VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; timeline.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    si.pNext=&timeline; si.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(c.physical,&si,&sp);
    if (!(sp.externalSemaphoreFeatures&VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
        throw std::runtime_error("Vulkan driver refuses a D3D12 timeline fence");
    c.create_device();
    // Query owns the once-per-session arithmetic admission. Feature creation
    // still validates imports/creates its device, but never dispatches those
    // probes again on first use, Apply, recovery or pipeline reconstruction.
    if (admitted) { caps=*admitted; return result; }
    caps.available=true; caps.path=Path::Portable;
    strcpy_s(caps.pathReason,"Portable KHR cooperative matrices; Radeon/Intel unvalidated");
    if(c.properties.vendorID==0x1002) {
        nrtest::ProbeResult probe;
        try { probe=nrtest::run_probes(c,root/"shaders"); }
        catch(...) { result.release();throw; } // uncertain probe work retains its device
        if (!probe.portable) throw std::runtime_error("Vulkan portable arithmetic probe refused this driver");
        caps.rdna3=probe.rdna3_layout && c.workgroup_layout.workgroupMemoryExplicitLayout &&
            c.workgroup_layout.workgroupMemoryExplicitLayout8BitAccess && c.workgroup_layout.workgroupMemoryExplicitLayout16BitAccess;
        if(caps.rdna3) { caps.path=Path::RDNA3; strcpy_s(caps.pathReason,"RDNA 3 fragment probe passed; Radeon game execution unvalidated"); }
        else strcpy_s(caps.pathReason,"RDNA 3 path refused by fragment/features probe; using portable KHR path");
    }
    return result;
}
VkInstance instance(DeviceContext* p){return p->context->instance;}
VkPhysicalDevice physical(DeviceContext* p){return p->context->physical;}
VkDevice device(DeviceContext* p){return p->context->device;}
VkQueue queue(DeviceContext* p){return p->context->queue;}
std::uint32_t family(DeviceContext* p){return p->context->family;}
}
