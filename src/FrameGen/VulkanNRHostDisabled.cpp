#include <PCH.h>
#include "VulkanNRHost.h"
#include "../PluginPaths.h"
namespace TheosRenderPipeline::NeuralRendering {
std::filesystem::path VulkanNRHost::Directory() { return PluginPaths::Directory()/L"TheosRenderPipeline"/L"VulkanNR"; }
std::filesystem::path VulkanNRHost::Path() { return Directory()/L"TRPVulkanNR.dll"; }
std::filesystem::path VulkanNRHost::Weights() { return PluginPaths::Directory()/L"TheosRenderPipeline"/L"NVIDIA"/L"nvngx_dlssnr_310.9.16.dll"; }
vknr::Capability VulkanNRHost::Probe(ID3D12Device*, bool, bool) { vknr::Capability c; strcpy_s(c.reason,kVendorEditionReason); return c; }
bool VulkanNRHost::Initialize(const FeatureSession::CreateInfo&) { status_=kVendorEditionReason; return false; }
bool VulkanNRHost::Evaluate(const FeatureSession::EvaluationInput&,SourceDLSSG::Interop&,ID3D12CommandQueue*,std::size_t,ID3D12GraphicsCommandList*&,bool,bool) { return false; }
bool VulkanNRHost::BeginFrame(std::size_t,ID3D12CommandQueue*,bool&) { return false; }
bool VulkanNRHost::Consume(const FeatureSession::EvaluationInput&,ID3D12CommandQueue*,std::size_t,ID3D12GraphicsCommandList*) { return false; }
bool VulkanNRHost::DropCompleted(ID3D12CommandQueue*) { return false; }
bool VulkanNRHost::ReleaseAfterRetirement() { return true; }
VulkanNRHost::~VulkanNRHost() = default;
}
