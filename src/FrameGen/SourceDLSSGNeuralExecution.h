#pragma once
#include "SourceDLSSGNeuralRendering.h"
#include "NeuralAsyncPipeline.h"

namespace TheosRenderPipeline::SourceDLSSG
{
// Host-thread owner. The worker owns a separate NeuralPass; no NGX parameters,
// descriptors or mutable options are shared with the presenting thread.
class NeuralExecution final
{
public:
    bool Record(ID3D12Device*, ID3D12GraphicsCommandList*, std::size_t,
        const NeuralOptions&, bool reset, bool depthInverted, float scaleX, float scaleY,
        ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* ui,
        ID3D12Resource* color, ID3D12Resource* composed, std::uint64_t frequency=0);
    HRESULT Submitted(ID3D12Fence*, std::uint64_t);
    bool RetireAsync() { return !async_ || async_->Stop(); }
    void RetireTelemetry() { if (regular_) regular_->RetireTelemetry(); }
    NeuralTelemetrySnapshot Telemetry() const { return regular_ ? regular_->Telemetry() : NeuralTelemetrySnapshot{}; }
    NeuralRendering::RuntimeBuild RetainedRuntimeBuild(const std::filesystem::path& path) const
    { return regular_ ? regular_->RetainedRuntimeBuild(path) : NeuralRendering::RuntimeBuild::Unknown; }
    bool NeedsRecreation(const NeuralOptions&, UINT width=0, UINT height=0, ID3D12Resource* scene=nullptr, DXGI_FORMAT motionFormat=DXGI_FORMAT_UNKNOWN) const;
    ID3D12Resource* Corrected() const { return corrected_; }
    ID3D12Resource* Composed() const { return regular_ ? regular_->Composed() : nullptr; }
    const std::string& Status() const { return status_; }
private:
    std::unique_ptr<NeuralPass> regular_;
    std::unique_ptr<TRPExperiment::AsyncPipeline> async_;
    NeuralOptions allocated_;
    UINT guideWidth_{}, guideHeight_{};
    D3D12_RESOURCE_DESC sceneDesc_{};
    DXGI_FORMAT motionFormat_{};
    bool initialized_{}, pendingSubmission_{}, skipped_{};
    ID3D12Resource* corrected_{}; // Host input or privately owned output, never worker data.
    std::string status_, fallback_;
};
}
