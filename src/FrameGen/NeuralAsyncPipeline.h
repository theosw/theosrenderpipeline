#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace TRPExperiment {
// Experimental worker queue. Regular NR remains the default.
// The caller owns COMMON-state, same-size scene/depth/motion resources and
// submits the recorded frame before calling Submitted. No inference wait is
// allowed on this path. One host frame may be outstanding; admission polls its
// fence rather than waiting. GPU readers own immutable snapshots until retired.
class AsyncPipeline {
public:
    struct Config {
        unsigned width{}, height{};
        unsigned guideWidth{}, guideHeight{}; // Zero means scene extent.
        DXGI_FORMAT motionFormat{DXGI_FORMAT_R32G32_FLOAT};
        DXGI_FORMAT format{DXGI_FORMAT_R16G16B16A16_FLOAT};
        unsigned maxAge{8};
        float depthTolerance{.05f}, colorTolerance{.5f}, maxRatio{2.f};
        float motionScaleX{1.f}, motionScaleY{1.f};
        std::uint64_t maxAllocationBytes{512ull*1024*1024};
        bool enabled{false};
    };
    struct Capture {
        ID3D12Resource *color{}, *motion{}, *depth{}, *output{};
        std::uint64_t frame{}, generation{};
        std::shared_ptr<const void> metadata; // Immutable options owned by this capture.
    };
    // Called ONLY on the worker. Inputs/output are COMMON on entry/return.
    // A failed recording is terminal, never submitted. Callback-owned features
    // must outlive Stop and must be retained if Stop cannot prove retirement.
    using Evaluator = std::function<bool(ID3D12GraphicsCommandList*, const Capture&)>;
    struct Snapshot {
        std::uint64_t frame{}, generation{}, evaluations{}, dropped{}, resultFrame{};
        unsigned resultAge{};
        bool displaying{}, failed{};
        double evaluationMs{};
        std::uint64_t allocationBytes{};
        std::string error;
    };

    AsyncPipeline();
    ~AsyncPipeline();
    AsyncPipeline(const AsyncPipeline&) = delete;
    AsyncPipeline& operator=(const AsyncPipeline&) = delete;
    HRESULT Initialize(ID3D12Device*, const Config&, Evaluator);
    HRESULT Record(ID3D12GraphicsCommandList*, ID3D12Resource* color,
        ID3D12Resource* motion, ID3D12Resource* depth, bool reset = false,
        bool enabled = true, std::shared_ptr<const void> metadata = {});
    HRESULT Submitted(ID3D12Fence*, std::uint64_t);
    ID3D12Resource* Output() const;
    Snapshot Status() const;
    // After all host submissions have completed. Joins bounded GPU waits;
    // does not release anything if completion was not proven.
    bool Stop();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
