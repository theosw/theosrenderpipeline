#pragma once
#include <Windows.h>
#include <atomic>
#include <new>
#include <thread>
#include <utility>

namespace TheosRenderPipeline
{
    // Optional screenshot work must not propagate exceptions into rendering or
    // escape a worker entry point. HRESULT failures remain visible to the owner.
    template<class Operation>
    HRESULT ScreenshotBoundary(Operation&& operation) noexcept
    {
        try { return operation(); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }

    // One encoder at a time. The owner must outlive its active operation (the
    // production backend is process-resident). No waiting on the Present thread.
    class ScreenshotWorker
    {
    public:
        bool Busy() const noexcept { return busy_.load(std::memory_order_acquire); }
        HRESULT TakeResult() noexcept
        {
            return Busy() ? S_FALSE : result_.exchange(S_FALSE);
        }
        template<class Operation>
        bool Start(Operation&& operation) noexcept
        {
            if (busy_.exchange(true)) { return false; }
            const auto hr = ScreenshotBoundary([&] {
                std::thread([this, task = std::forward<Operation>(operation)]() mutable noexcept {
                    result_.store(ScreenshotBoundary(task), std::memory_order_relaxed);
                    busy_.store(false, std::memory_order_release);
                }).detach();
                return S_OK;
            });
            if (FAILED(hr)) {
                result_ = hr;
                busy_.store(false, std::memory_order_release);
            }
            return SUCCEEDED(hr);
        }
    private:
        std::atomic_bool busy_{};
        std::atomic<HRESULT> result_{S_FALSE};
    };
}
