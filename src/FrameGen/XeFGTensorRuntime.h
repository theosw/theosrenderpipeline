#pragma once
#include "XeFGTensorAPI.h"
#include "XeFGPacingAPI.h"
#include <filesystem>
#include <string>

namespace TheosRenderPipeline
{
    // One optional extension per Intel presenter owner. A failed retirement keeps
    // its active flag and all modules/resources so another presenter cannot start.
    class XeFGTensorRuntime {
    public:
        bool Select(ID3D12Device*, const std::filesystem::path& intelDirectory, bool requested);
        HRESULT Start(ID3D12Device*, HMODULE fg, HMODULE ll);
        HRESULT Retire();
        HRESULT RegisterPacing(void*, ID3D12Device*, void (*logger)(const char*));
        HRESULT UnregisterPacing(void*);
        void ResetPacing(void* context) { if (pacing_.reset) pacing_.reset(context); }
        void SetFlipMetering(bool enabled) { meteringRequested_ = enabled; if (pacing_.setMetering) pacing_.setMetering(enabled); }
        void SetVSyncCadence(bool enabled) { if (vsyncCadenceRequested_ == enabled) return; vsyncCadenceRequested_ = enabled; if (pacing_.setVSyncCadence) pacing_.setVSyncCadence(enabled); }
        bool FlipMeteringRequested() const { return meteringRequested_; }
        XeFGPacing::Stats PacingStats() const;
        const std::string& PacingStatus() const { return pacingStatus_; }
        bool Selected() const { return module_ != nullptr; }
        bool ModelReady() const { return (Snapshot().flags & XeFGTensor::ModelReady) != 0; }
        bool Unsafe() const { return (Snapshot().flags & XeFGTensor::Unsafe) != 0 || (PacingStats().flags & XeFGPacing::Unsafe) != 0; }
        XeFGTensor::Stats Snapshot() const;
        const std::filesystem::path& Directory() const { return directory_; }
        const std::string& Status() const { return status_; }
    private:
        HMODULE module_{};
        XeFGTensor::API api_{};
        XeFGPacing::API pacing_{};
        std::string pacingStatus_{"XeFG pacing extension unavailable; SDK pacing retained"};
        bool meteringRequested_{true};
        bool vsyncCadenceRequested_{};
        std::filesystem::path directory_;
        std::string status_{"Tensor backend off"};
        bool active_{};
        bool unsafe_{};
    };
}
