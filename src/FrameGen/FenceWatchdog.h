#pragma once
// Releases a D3D12 queue that is GPU-waiting on a shared fence which its
// external producer (Vulkan NR) never signals. Without it a lost or hung
// Vulkan device would hold the game's queue, and with it the game, forever.
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace TheosRenderPipeline::NeuralRendering {
// Interrupt time without sleep and hibernation, so a PC that sleeps mid-frame
// does not wake up to a timeout it never really waited for.
struct AwakeClock {
    using rep = long long;
    using period = std::ratio<1, 10'000'000>;  // 100 ns units
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<AwakeClock>;
    static constexpr bool is_steady = true;
    static time_point now() noexcept {
        ULONGLONG ticks{};
        QueryUnbiasedInterruptTime(&ticks);
        return time_point(duration(static_cast<rep>(ticks)));
    }
};

class FenceWatchdog {
public:
    using Clock = AwakeClock;
    FenceWatchdog() = default;
    FenceWatchdog(const FenceWatchdog&) = delete;
    FenceWatchdog& operator=(const FenceWatchdog&) = delete;
    ~FenceWatchdog() { Stop(); }

    // `timeout` is measured from the last observed fence progress, so a
    // pipelined queue that keeps completing work never fires.
    void Start(ID3D12Fence* fence, std::chrono::milliseconds timeout) {
        Stop();
        fence_ = fence; timeout_ = timeout; fired_ = false; stop_ = false;
        armed_ = 0; firedValue_ = 0; completed_ = fence->GetCompletedValue();
        thread_ = std::thread([this] { Run(); });
    }
    void Stop() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
        fence_.Reset();
    }
    // Call after queueing a GPU wait for `value` that only the external producer signals.
    void Arm(std::uint64_t value) {
        {
            std::lock_guard lock(mutex_);
            if (value <= armed_) return;
            if (armed_ <= completed_) progress_ = Clock::now();  // idle -> armed
            armed_ = value;
        }
        wake_.notify_all();
    }
    // The producer has reported a failure (e.g. Vulkan device lost): release
    // any held queue now instead of waiting out the timeout. True if a wait was released.
    bool ReleaseNow() {
        std::lock_guard lock(mutex_);
        if (!fence_ || fired_) return false;
        const auto completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX || completed >= armed_) return false;
        ReleaseLocked();
        return true;
    }
    bool Fired() const { return fired_.load(std::memory_order_acquire); }
    std::uint64_t FiredValue() const { return firedValue_.load(std::memory_order_acquire); }
    std::chrono::milliseconds Timeout() const { return timeout_; }

private:
    void ReleaseLocked() {
        const auto value = armed_;
        // CPU signal unblocks every queued GPU wait up to `value`.
        // The producer is treated as failed for the session afterwards.
        fence_->Signal(value);
        completed_ = value;
        firedValue_.store(value, std::memory_order_release);
        fired_.store(true, std::memory_order_release);
    }
    void Run() {
        std::unique_lock lock(mutex_);
        while (!stop_) {
            if (fired_ || armed_ <= completed_) {
                wake_.wait(lock, [this] { return stop_ || (!fired_ && armed_ > completed_); });
                continue;
            }
            wake_.wait_for(lock, (std::min)(timeout_ / 8, std::chrono::milliseconds(100)) + std::chrono::milliseconds(1));
            if (stop_) break;
            if (fired_) continue;
            const auto completed = fence_->GetCompletedValue();
            const auto now = Clock::now();
            if (completed == UINT64_MAX) { completed_ = armed_; continue; }  // device removed: nothing to release
            if (completed > completed_) { completed_ = completed; progress_ = now; }
            if (armed_ > completed_ && now - progress_ >= timeout_) ReleaseLocked();
        }
    }
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::chrono::milliseconds timeout_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::uint64_t armed_{}, completed_{};
    Clock::time_point progress_{};
    std::atomic_bool fired_{};
    std::atomic<std::uint64_t> firedValue_{};
    bool stop_{true};
};
}
