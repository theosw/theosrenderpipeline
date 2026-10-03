#include "FrameGen/SourceDLSSGInterop.h"
#include <dxgi1_4.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline::SourceDLSSG;

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

int main(int argc, char** argv)
{
    const bool requireWrapped = argc == 2 && std::string_view(argv[1]) == "--require-wrapped";
    Require(argc == 1 || requireWrapped, "supported arguments");
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create factory");
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device11, nullptr, &context11), "create hardware D3D11 device");
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    Check(device11.As(&dxgi), "query DXGI device");
    Check(dxgi->GetAdapter(&adapter), "matching adapter");
    ComPtr<ID3D12Device> device12;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12)), "matching D3D12 device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "create presenting queue");
    ComPtr<ID3D12Fence> input;
    Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&input)), "create input completion fence");
    ComPtr<ID3D12Device> inputOwner;
    ComPtr<IUnknown> hostIdentity, inputOwnerIdentity;
    Check(input->GetDevice(IID_PPV_ARGS(&inputOwner)), "input fence owner");
    Check(device12.As(&hostIdentity), "host identity");
    Check(inputOwner.As(&inputOwnerIdentity), "input fence owner identity");
    std::printf("hostIdentity=%p inputFenceOwner=%p wrapped=%u\n", hostIdentity.Get(), inputOwnerIdentity.Get(),
        hostIdentity != inputOwnerIdentity);
    Require(!requireWrapped || hostIdentity != inputOwnerIdentity,
        "ReShade regression mode must actually expose different proxy/native identities");

    Interop interop;
    Check(interop.Initialize(device11.Get(), device12.Get(), queue.Get()), "initialize production interop");
    Check(interop.WaitForInputReaders(nullptr, 0), "no-fence presenting queue bridge");
    const auto beforeInvalid = interop.LastValue(Work::FrameGeneration);
    Require(interop.WaitForInputReaders(nullptr, 1) == E_UNEXPECTED, "missing nonzero fence rejected");
    Require(std::string_view(interop.LastInputWait().stage) == "preconditions", "precondition diagnostic");
    Require(interop.LastValue(Work::FrameGeneration) == beforeInvalid, "invalid input enqueues no bridge");
    Check(interop.WaitForInputReaders(input.Get(), 0), "same-device zero-valued fence");
    Check(queue->Signal(input.Get(), 1), "signal input completion");
    Check(interop.WaitForInputReaders(input.Get(), 1), "same-device signaled fence");
    Require(std::string_view(interop.LastInputWait().stage) == "complete", "completed wait diagnostic");
    Require(interop.LastInputWait().inputFenceOwner == inputOwnerIdentity.Get(), "diagnostic owner identity");
    Check(interop.Drain(), "retire valid input waits");

    // An actual device on a different adapter, not just a fabricated pointer.
    ComPtr<IDXGIAdapter> warp;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "get foreign WARP adapter");
    ComPtr<ID3D12Device> foreignDevice;
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&foreignDevice)), "create foreign device");
    ComPtr<ID3D12Fence> foreignFence;
    Check(foreignDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreignFence)), "create foreign fence");
    const auto beforeForeign = interop.LastValue(Work::FrameGeneration);
    Require(interop.WaitForInputReaders(foreignFence.Get(), 0) == E_INVALIDARG, "foreign zero-valued fence rejected");
    Require(interop.WaitForInputReaders(foreignFence.Get(), 9) == E_INVALIDARG, "foreign pending fence rejected");
    Require(std::string_view(interop.LastInputWait().stage) == "device identity", "foreign-device diagnostic");
    Require(interop.LastValue(Work::FrameGeneration) == beforeForeign, "foreign fence enqueues no wait/signal");
    Require(interop.Ready(), "validation rejection preserves existing interop fault policy");

    // A still-pending legitimate input must block the presenting queue. Complete
    // it from the CPU only after observing that work behind the wait cannot run.
    Check(interop.WaitForInputReaders(input.Get(), 2), "enqueue pending legitimate input wait");
    ComPtr<ID3D12Fence> progress;
    Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&progress)), "create progress fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "create progress event");
    Check(progress->SetEventOnCompletion(1, event), "watch work after input wait");
    Check(queue->Signal(progress.Get(), 1), "enqueue work after input wait");
    Require(WaitForSingleObject(event, 20) == WAIT_TIMEOUT, "pending input prevents later queue progress");
    Check(input->Signal(2), "release pending input from CPU");
    Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "queue resumes after real input completion");
    CloseHandle(event);
    Check(interop.Drain(), "retire completed input bridge");
    Check(interop.SignalD3D11(Work::Upscaling), "D3D11 producer resumes after bridge");
    Check(interop.Drain(), "retire resumed D3D11 producer");

    ID3D12GraphicsCommandList* list = nullptr;
    Check(interop.Begin(Work::FrameGeneration, &list), "begin frame recording");
    Require(interop.WaitForInputReaders(input.Get(), 2) == E_UNEXPECTED, "input reuse during recording rejected");
    Check(interop.Submit(Work::FrameGeneration), "submit recorded frame");
    Check(interop.Drain(), "retire final frame");

    // Fill every command slot behind queue work gated on the input fence, so
    // the next Begin must wait for a submission the GPU has not yet reached.
    RetirementWaitDiagnostics wait;
    Require(!interop.TakeExtendedWait(wait), "prompt retirement records no extended wait");
    auto fillSlots = [&](Interop& target) {
        for (std::size_t slot = 0; slot < kCommandSlots; ++slot) {
            ID3D12GraphicsCommandList* filled = nullptr;
            Check(target.Begin(Work::FrameGeneration, &filled), "begin gated slot");
            Check(target.Submit(Work::FrameGeneration), "submit gated slot");
        }
    };
    auto release = [&](std::uint64_t first, std::uint64_t last, int gapMs) {
        return std::thread([&input, first, last, gapMs] {
            for (auto value = first; value <= last; ++value) {
                std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
                input->Signal(value);
            }
        });
    };

    // A stall shorter than the limit is a slow GPU, not a fault.
    interop.SetRetirementWaitPolicy({ 20, 1000 });
    Check(interop.WaitForInputReaders(input.Get(), 3), "gate queue for short stall");
    fillSlots(interop);
    auto shortStall = release(3, 3, 150);
    list = nullptr;
    Check(interop.Begin(Work::FrameGeneration, &list), "begin recovers after short stall");
    shortStall.join();
    Require(list && interop.Ready(), "short stall leaves interop usable");
    Require(interop.TakeExtendedWait(wait) && wait.work == Work::FrameGeneration && wait.slices > 0 &&
        wait.result == S_OK && wait.completedAtEnd >= wait.target, "short stall is reported as an extended wait");
    Check(interop.Submit(Work::FrameGeneration), "submit after short stall");
    Check(interop.Drain(), "retire short stall");

    // Steady progress keeps waiting beyond the no-progress limit. Timed-out
    // slices accumulate well past the limit, so only a progress reset passes.
    interop.SetRetirementWaitPolicy({ 20, 150 });
    for (std::uint64_t gate = 4; gate <= 11; ++gate) {
        Check(interop.WaitForInputReaders(input.Get(), gate), "gate queue for slow progress");
    }
    fillSlots(interop);
    auto slowProgress = release(4, 11, 50);
    list = nullptr;
    Check(interop.Begin(Work::FrameGeneration, &list), "begin waits through slow progress");
    slowProgress.join();
    Require(list && interop.Ready(), "slow progress leaves interop usable");
    // Windows rounds 20 ms slices and 50 ms gaps up to its timer granularity
    // (about 31 and 62 ms by default), so nominal slices x 20 is not elapsed time.
    // Without progress resets the wait would fail after 150 / 20 timed-out
    // slices; exceeding that count and twice the limit in real time proves them.
    Require(interop.TakeExtendedWait(wait), "slow progress is reported as an extended wait");
    std::printf("slow progress slices=%u elapsedMs=%llu\n", wait.slices, static_cast<unsigned long long>(wait.elapsedMs));
    Require(wait.result == S_OK && wait.slices > 150 / 20 && wait.elapsedMs > 2 * 150 &&
        wait.completedAtEnd > wait.completedAtStart, "slow progress outlasts the no-progress limit");
    Check(interop.Submit(Work::FrameGeneration), "submit after slow progress");
    Check(interop.Drain(), "retire slow progress");

    // A frozen fence still fails, without resetting the in-flight slot.
    {
        Interop frozen;
        Check(frozen.Initialize(device11.Get(), device12.Get(), queue.Get()), "initialize frozen interop");
        frozen.SetRetirementWaitPolicy({ 20, 100 });
        Check(frozen.WaitForInputReaders(input.Get(), 12), "gate queue indefinitely");
        fillSlots(frozen);
        const auto slot = frozen.CurrentSlot(Work::FrameGeneration);
        const auto value = frozen.LastValue(Work::FrameGeneration);
        list = nullptr;
        const auto frozenResult = frozen.Begin(Work::FrameGeneration, &list);
        Require(frozenResult == HRESULT_FROM_WIN32(WAIT_TIMEOUT) && !list, "frozen fence times out without a list");
        Require(!frozen.Ready() && frozen.Fault() == HRESULT_FROM_WIN32(WAIT_TIMEOUT), "frozen timeout latches the fault");
        Require(frozen.CurrentSlot(Work::FrameGeneration) == slot && frozen.LastValue(Work::FrameGeneration) == value,
            "frozen timeout keeps the in-flight slot");
        Require(frozen.TakeExtendedWait(wait) && wait.result == HRESULT_FROM_WIN32(WAIT_TIMEOUT) &&
            wait.completedAtEnd == wait.completedAtStart && wait.completedAtEnd < wait.target, "frozen wait records no progress");
        const auto sameWait = [](const RetirementWaitDiagnostics& a, const RetirementWaitDiagnostics& b) {
            return a.work == b.work && a.target == b.target && a.completedAtStart == b.completedAtStart &&
                a.completedAtEnd == b.completedAtEnd && a.elapsedMs == b.elapsedMs && a.slices == b.slices && a.result == b.result;
        };
        const auto firstFailure = frozen.LastFailure();
        Require(firstFailure.valid && firstFailure.result == frozenResult &&
            firstFailure.work == Work::FrameGeneration && firstFailure.slot == slot &&
            firstFailure.completedAvailable && firstFailure.waitPerformed && firstFailure.waitResult == WAIT_TIMEOUT &&
            firstFailure.waitPolicy.sliceMs == 20 && firstFailure.waitPolicy.stallLimitMs == 100 &&
            std::string_view(firstFailure.stage) == "fence stalled retirement",
            "first-failure report retains the operation and policy of the wait that actually timed out");
        Require(sameWait(firstFailure.wait, wait), "first-failure report holds the wait's own record, not a copy that can drift");
        Check(input->Signal(12), "release frozen queue");
        Check(frozen.Drain(), "frozen work retires after release");
        Require(frozen.LastFailure().stage == firstFailure.stage && frozen.LastFailure().result == firstFailure.result &&
            sameWait(frozen.LastFailure().wait, firstFailure.wait),
            "successful later retirement preserves the original frozen-fence evidence");
    }
    std::puts("PASS: same-device input fences, foreign-device rejection, pending GPU wait, retirement, extended waits and diagnostics");
    return 0;
}
