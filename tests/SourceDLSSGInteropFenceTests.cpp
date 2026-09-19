#include "FrameGen/SourceDLSSGInterop.h"
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <string_view>

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
    std::puts("PASS: same-device input fences, foreign-device rejection, pending GPU wait, retirement and diagnostics");
    return 0;
}
