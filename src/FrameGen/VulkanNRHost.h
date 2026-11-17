#pragma once
#include "NeuralRenderingFeatureSession.h"
#include "SourceDLSSGInterop.h"
#include "FenceWatchdog.h"
#include "../../third_party/vknr/include/vknr.h"
#include <array>
namespace TheosRenderPipeline::NeuralRendering {
class VulkanNRHost {
public:
    static std::filesystem::path Directory();
    static std::filesystem::path Path();
    static std::filesystem::path Weights();
    // Passive by default: only explicit selection/initialization may request a probe.
    static vknr::Capability Probe(ID3D12Device*, bool wait = false, bool request = false);
    bool Initialize(const FeatureSession::CreateInfo&);
    bool Evaluate(const FeatureSession::EvaluationInput&, SourceDLSSG::Interop&, ID3D12CommandQueue*,
        std::size_t slot, ID3D12GraphicsCommandList*&, bool background=false, bool frameOpened=false);
    bool BeginFrame(std::size_t slot, ID3D12CommandQueue*, bool& ready);
    bool Consume(const FeatureSession::EvaluationInput&, ID3D12CommandQueue*, std::size_t slot, ID3D12GraphicsCommandList*);
    bool DropCompleted(ID3D12CommandQueue* queue);
    VulkanNRMeasurements Measurements() const { return measurements_; }
    bool ReleaseAfterRetirement();
    bool Safe() const { return !unsafe_; }
    const std::string& Status() const { return status_; }
    ~VulkanNRHost();
private:
    bool Shaders(ID3D12Device*);
    bool WatchdogFired();
    void Dispatch(ID3D12GraphicsCommandList*, std::size_t, const FeatureSession::EvaluationInput&, bool);
    vknr::Handle handle_{};
    vknr::Capability capability_{};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,4> images_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pack_, unpack_;
    std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>,vknr::Slots> heaps_;
    std::uint64_t value_{}, frames_{};
    std::uint32_t width_{},height_{},guideWidth_{},guideHeight_{};
    bool unsafe_{};
    bool backgroundPending_{},backgroundReady_{};
    VulkanNRMeasurements measurements_{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
    Microsoft::WRL::ComPtr<ID3D12Resource> queryReadback_;
    std::uint64_t* mappedQueries_{};
    std::uint64_t queryFrequency_{};
    std::array<std::array<bool,2>,vknr::Slots> queryPending_{};
    std::string status_;
    FenceWatchdog watchdog_; // last member: stops before the fence and images go
};
}
