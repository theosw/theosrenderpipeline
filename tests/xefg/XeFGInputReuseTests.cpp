#include "FrameGen/SourceDLSSGInterop.h"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline::SourceDLSSG;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); ExitProcess(1); }
}
static void Check(HRESULT result, const char* reason)
{
    if (FAILED(result)) {
        std::fprintf(stderr, "FAIL: %s HRESULT=0x%08lX\n", reason, static_cast<unsigned long>(result));
        ExitProcess(1);
    }
}

int main(int argc, char** argv)
{
    const bool omitWait = argc == 2 && std::string_view(argv[1]) == "--omit-input-wait";
    Require(argc == 1 || omitWait, "supported mutation-control argument");
    // The SDK's ONLY_NOW operation is a copy into its own resources. Model
    // that read with an actual cross-API texture copy, held behind a GPU gate,
    // then overwrite the singleton from D3D11. No CPU retirement may hide the
    // race. A second gate models later SDK work that does not read the input.
    ComPtr<ID3D12Debug> debug;
    Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug layer");
    debug->EnableDebugLayer();
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device11, nullptr, &context11), "D3D11 device");
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    Check(device11.As(&device5), "D3D11 fences");
    Check(context11.As(&context4), "D3D11 GPU waits");
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    Check(device11.As(&dxgi), "DXGI device");
    Check(dxgi->GetAdapter(&adapter), "matching adapter");
    ComPtr<ID3D12Device> device12;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12)), "D3D12 device");
    ComPtr<ID3D12InfoQueue> messages;
    Check(device12.As(&messages), "debug messages");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> queue;
    Check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
    Interop interop;
    Check(interop.Initialize(device11.Get(), device12.Get(), queue.Get()), "production interop");

    constexpr UINT width = 64, height = 32;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    SharedTexture input, copied;
    Check(interop.CreateSharedTexture(desc, input), "singleton input");
    Check(interop.CreateSharedTexture(desc, copied), "reader's private copy");
    ComPtr<ID3D11Texture2D> source, staging;
    Check(device11->CreateTexture2D(&desc, nullptr, &source), "producer texture");
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.BindFlags = 0;
    Check(device11->CreateTexture2D(&desc, nullptr, &staging), "readback texture");
    auto paint = [&](UINT pixel) {
        const std::vector<UINT> pixels(width * height, pixel);
        context11->UpdateSubresource(source.Get(), 0, nullptr, pixels.data(), width * sizeof(UINT), 0);
        Check(interop.CopyInput(source.Get(), input), "overwrite singleton");
    };
    auto verify = [&](ID3D11Texture2D* texture, UINT expected) {
        context11->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context11->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback");
        for (UINT y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const UINT*>(static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < width; ++x) Require(row[x] == expected, "reader preserves the previous frame's pixels");
        }
        context11->Unmap(staging.Get(), 0);
    };

    ComPtr<ID3D12Fence> copyGate, tailGate, tailProgress;
    ComPtr<ID3D11Fence> producerProgress;
    Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyGate)), "copy gate");
    Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tailGate)), "later-work gate");
    Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tailProgress)), "later-work progress");
    Check(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&producerProgress)), "producer progress");
    HANDLE producerEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE tailEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(producerEvent != nullptr, "producer event");
    Require(tailEvent != nullptr, "later-work event");

    for (UINT64 frame = 1; frame <= 12; ++frame) {
        const UINT oldPixel = 0xFF003000u + static_cast<UINT>(frame);
        const UINT newPixel = 0xFF700000u + static_cast<UINT>(frame);
        paint(oldPixel);
        Check(interop.SignalD3D11(Work::SwapChain), "publish input");
        Check(interop.Drain(), "retire initial producer only");
        Check(queue->Wait(copyGate.Get(), frame), "hold input reader");
        ID3D12GraphicsCommandList* list{};
        Check(interop.Begin(Work::SwapChain, &list), "begin copy");
        Check(Interop::RecordCopy(list, input.texture12.Get(), copied.texture12.Get()), "record input reader");
        Check(interop.Submit(Work::SwapChain), "submit copy and completion fence");
        Check(queue->Wait(tailGate.Get(), frame), "hold later SDK work");
        Check(queue->Signal(tailProgress.Get(), frame), "watch later SDK work");

        // This must return while the reader is still gated. Drain here would
        // stall the CPU, and a wait on a later Present signal would over-wait.
        if (!omitWait) Check(interop.WaitD3D11(Work::SwapChain), "GPU-only input reuse boundary");
        Require(copyGate->GetCompletedValue() < frame, "boundary returned before reader completion");
        paint(newPixel);
        Check(context4->Signal(producerProgress.Get(), frame), "observe next producer");
        Check(producerProgress->SetEventOnCompletion(frame, producerEvent), "watch producer");
        context11->Flush();
        Require(WaitForSingleObject(producerEvent, 30) == WAIT_TIMEOUT, "producer cannot overwrite pending input reader");

        Check(copyGate->Signal(frame), "release input copy");
        Require(WaitForSingleObject(producerEvent, 3000) == WAIT_OBJECT_0, "producer resumes when copies finish");
        Require(tailProgress->GetCompletedValue() < frame, "input reuse does not wait for unrelated later work");
        Check(tailGate->Signal(frame), "release later work");
        Check(tailProgress->SetEventOnCompletion(frame, tailEvent), "watch later-work retirement");
        Require(WaitForSingleObject(tailEvent, 3000) == WAIT_OBJECT_0, "retire test-owned later work");
        Check(interop.Drain(), "lifecycle retirement before readback/reuse");
        verify(copied.texture11.Get(), oldPixel);
        verify(input.texture11.Get(), newPixel);
    }
    CloseHandle(producerEvent);
    CloseHandle(tailEvent);
    Check(interop.Drain(), "final retirement before resource destruction");
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{}; messages->GetMessage(i, nullptr, &size);
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        messages->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr, "%s\n", message->pDescription);
            Require(false, "D3D12 debug errors");
        }
    }
    std::puts("PASS: CPU returns with copies pending; D3D11 overwrites wait for copies, not later SDK work; all pixels preserved across allocator wrap");
}
