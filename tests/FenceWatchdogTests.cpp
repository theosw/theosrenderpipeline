// The Vulkan NR handoff queues a D3D12 GPU wait that only Vulkan signals.
// These tests prove the watchdog releases such a wait when the producer never
// signals, and leaves a progressing or idle fence alone.
#include "FrameGen/FenceWatchdog.h"
#include <dxgi1_4.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::NeuralRendering::FenceWatchdog;
using namespace std::chrono_literals;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

static void Check(HRESULT hr, const char* reason)
{
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: %s HRESULT=0x%08lX\n", reason, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

static bool WaitFor(ID3D12Fence* fence, std::uint64_t value, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (fence->GetCompletedValue() < value) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

int main()
{
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create factory");
    ComPtr<IDXGIAdapter> warp;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP D3D12 device");
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");

    {   // A producer that never signals: the queue is released after the timeout.
        ComPtr<ID3D12Fence> shared, after;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared)), "shared fence");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&after)), "after fence");
        FenceWatchdog watchdog;
        watchdog.Start(shared.Get(), 200ms);
        Check(queue->Wait(shared.Get(), 2), "queue waits for the producer");
        Check(queue->Signal(after.Get(), 1), "work behind the wait");
        watchdog.Arm(2);
        std::this_thread::sleep_for(50ms);
        Require(after->GetCompletedValue() == 0 && !watchdog.Fired(), "queue is held before the timeout");
        Require(WaitFor(after.Get(), 1, 3000ms), "watchdog released the held queue");
        Require(watchdog.Fired() && watchdog.FiredValue() == 2, "watchdog reports the released value");
        Require(shared->GetCompletedValue() == 2, "shared fence was signalled to the armed value");
        std::puts("released stalled producer");
    }
    {   // A producer that keeps completing: never fires, even across many timeouts.
        ComPtr<ID3D12Fence> shared;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared)), "shared fence");
        FenceWatchdog watchdog;
        watchdog.Start(shared.Get(), 150ms);
        for (std::uint64_t value = 1; value <= 12; ++value) {
            watchdog.Arm(value + 1);  // the next frame is queued before this one completes
            std::this_thread::sleep_for(60ms);
            Check(shared->Signal(value), "producer completes a frame");
        }
        Check(shared->Signal(13), "producer completes the last frame");
        std::this_thread::sleep_for(300ms);
        Require(!watchdog.Fired(), "progressing producer is left alone");
        std::puts("left progressing producer alone");
    }
    {   // Idle (never armed, or caught up) never fires, and Stop returns promptly while armed.
        ComPtr<ID3D12Fence> shared;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared)), "shared fence");
        FenceWatchdog watchdog;
        watchdog.Start(shared.Get(), 100ms);
        std::this_thread::sleep_for(300ms);
        Require(!watchdog.Fired(), "idle watchdog does not fire");
        watchdog.Start(shared.Get(), 10s);
        watchdog.Arm(5);
        const auto stopped = std::chrono::steady_clock::now();
        watchdog.Stop();
        Require(std::chrono::steady_clock::now() - stopped < 1s, "Stop does not wait out the timeout");
        Require(!watchdog.Fired() && shared->GetCompletedValue() == 0, "stopped watchdog leaves the fence untouched");
        std::puts("idle and stop");
    }
    std::puts("PASS");
    return 0;
}
