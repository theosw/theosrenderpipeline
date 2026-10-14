#pragma once
// Optional native producer for the isolated XPAR presenter. No ReShade hooks.
#include "shared/protocol.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <string>

namespace TheosRenderPipeline::FrameWarp {
struct Status {
    bool requested{}, original{}, active{}, faulted{};
    std::uint64_t published{}, dropped{};
    float sourceFPS{}, outputFPS{}, warpGPUMs{}, sourceAgeMs{};
    bool calibratedX{}, calibratedY{};
    std::uint64_t copyGPUSamples{};
    double copyGPUAverageMs{}, copyGPUMaxMs{};
    std::string message{"disabled"};
};
class Capture final {
public:
    Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture();
    void Enable(bool enabled) { requested_ = enabled; if (!enabled) Suspend("disabled"); }
    bool Requested() const { return requested_; }
    void Original(bool enabled) { original_ = enabled; }
    bool Original() const { return original_; }
    Status Snapshot() const;
    // All inputs are COMMON on the host queue. Camera sample time is known;
    // simulation-start is intentionally unknown, rather than manufactured.
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, HWND window,
        const std::array<ID3D12Resource*, 5>& inputs, const fw::Camera& camera,
        std::int64_t cameraQpc, bool eligible, const std::filesystem::path& helper = {});
    void Submitted(ID3D12CommandQueue* queue);
    void Suspend(const char* reason);
    // Only pass true after the host queue has successfully drained. On failure
    // retain recorded resources, including a copy whose Signal failed.
    bool Reset(bool hostRetired = false);
    fw::Shared* SharedForTests() const { return shared_; }
private:
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Slot { std::array<Resource, 5> textures; std::array<HANDLE, 5> handles{};
        std::array<std::uint32_t, 5> generations{}; std::uint64_t submitted{}; bool timestampPending{}; };
    bool Open(ID3D12Device* device, HWND window, const D3D12_RESOURCE_DESC& color);
    bool Allocate(ID3D12Device* device, int slot, const std::array<ID3D12Resource*, 5>& inputs);
    bool Launch(const std::filesystem::path& helper);
    void Fail(const char* reason);
    void Collect(std::uint64_t completed);
    std::array<Slot, fw::kSlots> slots_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamps_;
    Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback_;
    std::uint64_t gpuFrequency_{}, copyGPUSamples_{};
    double copyGPUMs_{}, copyGPUMaxMs_{};
    HANDLE mapping_{}, fenceHandle_{}, job_{}, process_{};
    fw::Shared* shared_{};
    int pending_{-1};
    std::uint64_t nextFence_{}, nextFrame_{}, published_{}, dropped_{};
    std::uint32_t epoch_{1};
    bool requested_{}, original_{}, active_{}, faulted_{}, resetCamera_{true};
    std::string message_{"disabled"};
};
}
