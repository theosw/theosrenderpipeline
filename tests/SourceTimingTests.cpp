#include "PerformanceTuning.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <thread>
#include <chrono>
#include <d3d11sdklayers.h>
#include <vector>

static void Require(bool ok, const char* text)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); } }

int main()
{
    using Stage = PerformanceTuning::D3D11Stage;
    static_assert(static_cast<unsigned>(Stage::kOutputCopy) == 6);
    static_assert(static_cast<unsigned>(Stage::kNeuralEarlyRoundTrip) == 10);
    static_assert(static_cast<unsigned>(Stage::kPresentationCopy) == 11);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "WARP device");
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> info;
    Require(SUCCEEDED(device.As(&info)), "debug queue");
    auto* timing = PerformanceTuning::GetSingleton();
    Require(timing->TimingEnabled(), "startup measurements enabled by default");
    PerformanceTuning::Settings settings;
    settings.enableGPUTimings = false;
    timing->ApplySettings(settings);
    timing->RecordSourcePresentCpuMs(10);
    timing->RecordNeuralEarlyCPU(4'000'000, 1'000'000, true);
    Require(timing->GetTimingSnapshot().sourcePresentCpu.samples == 0, "disabled CPU timing absent");
    Require(timing->GetTimingSnapshot().neuralEarly.samples == 0, "disabled NR timing absent");
    settings.enableGPUTimings = true;
    timing->ApplySettings(settings);
    for (int i = 1; i <= 30; ++i) { timing->RecordSourcePresentCpuMs(static_cast<float>(i)); }
    const auto cpu = timing->GetTimingSnapshot().sourcePresentCpu;
    Require(cpu.samples == 30 && std::abs(cpu.p50Ms - 15.5f) < 0.001f &&
        std::abs(cpu.p95Ms - 28.55f) < 0.001f, "CPU percentile distribution");
    timing->RecordSourcePresentCpuMs(-1);
    timing->RecordSourcePresentCpuMs(std::numeric_limits<float>::infinity());
    Require(timing->GetTimingSnapshot().sourcePresentCpu.samples == 30, "invalid CPU samples rejected");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 128; desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
        SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv)), "GPU workload");
    Microsoft::WRL::ComPtr<ID3D11Texture2D> handoff, presentation;
    Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &handoff)) &&
        SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &presentation)), "copy destinations");
    auto frame = [&](bool ui, unsigned id, bool outputCopy = true, bool presentationCopy = true) {
        timing->BeginD3D11Frame(device.Get(), context.Get(), id);
        if (outputCopy) {
            ScopedD3D11PerformanceStage stage{context.Get(), Stage::kOutputCopy};
            context->CopyResource(handoff.Get(), texture.Get());
        }
        if (presentationCopy) {
            ScopedD3D11PerformanceStage stage{context.Get(), Stage::kPresentationCopy};
            context->CopyResource(presentation.Get(), handoff.Get());
        }
        if (ui) {
            ScopedD3D11PerformanceStage stage{context.Get(), Stage::kNativeUIComposition};
            const float color[]{0.25f, 0.5f, 0.75f, 1};
            context->ClearRenderTargetView(rtv.Get(), color);
            ScopedD3D11PerformanceStage nr{context.Get(), Stage::kNeuralEarlyRoundTrip};
            context->ClearRenderTargetView(rtv.Get(), color);
            timing->RecordNeuralEarlyCPU(4'000'000, 1'000'000, true);
            timing->RecordNeuralEarlyCPU(99'000'000, 0, false); // Duplicate must not overwrite the paired sample.
        }
        timing->EndD3D11Frame(context.Get());
        context->Flush(); // Fixture only: production query polling never flushes or waits.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    for (unsigned i = 0; i < 200 && !timing->GetTimingSnapshot().d3d11Available[8]; ++i) { frame(true, i); }
    Require(timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kFrame)], "retired GPU frame available");
    Require(timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kOutputCopy)] &&
        timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kPresentationCopy)],
        "both output copies retire independently in the same frame");
    Require(timing->GetTimingSnapshot().d3d11Available[8] &&
        std::isfinite(timing->GetTimingSnapshot().d3d11Ms[8]), "native UI stage retired and finite");
    const auto nr = timing->GetTimingSnapshot().neuralEarly;
    Require(nr.samples > 0 && nr.cpuNanoseconds == nr.samples * 4'000'000 &&
        nr.allocatorWaitNanoseconds == nr.samples * 1'000'000 && nr.allocatorWaits == nr.samples &&
        nr.cpuMaximumNanoseconds == 4'000'000 && nr.allocatorWaitMaximumNanoseconds == 1'000'000,
        "NR CPU/wait totals pair with retired GPU samples, exactly once");
    Require(timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kNeuralEarlyRoundTrip)],
        "NR round-trip GPU interval retires");
    for (unsigned i = 0; i < 30; ++i) { frame(false, 300 + i); }
    Require(!timing->GetTimingSnapshot().d3d11Available[8], "absent stage cannot retain stale availability");
    Require(!timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kNeuralEarlyRoundTrip)],
        "disabled/absent NR cannot retain a current GPU interval");
    const auto inactiveSamples = timing->GetTimingSnapshot().neuralEarly.samples;
    timing->RecordNeuralEarlyCPU(99'000'000, 0, false);
    for (unsigned i = 0; i < 10; ++i) { frame(false, 350 + i); }
    Require(timing->GetTimingSnapshot().neuralEarly.samples == inactiveSamples,
        "NR absence and an out-of-frame CPU record cannot add samples");
    for (unsigned i = 0; i < 30; ++i) { frame(false, 370 + i, false); }
    Require(!timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kOutputCopy)] &&
        timing->GetTimingSnapshot().d3d11Ms[static_cast<size_t>(Stage::kOutputCopy)] == 0.0f &&
        timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kPresentationCopy)],
        "direct output removes only the backend copy, with no stale timing");
    for (unsigned i = 0; i < 30; ++i) { frame(false, 410 + i, true, false); }
    Require(timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kOutputCopy)] &&
        !timing->GetTimingSnapshot().d3d11Available[static_cast<size_t>(Stage::kPresentationCopy)],
        "presentation absence does not hide an intermediate copy");
    frame(true, 400);
    timing->ResetTimingWindow();
    settings.enableGPUTimings = false;
    timing->ApplySettings(settings);
    for (unsigned i = 0; i < 10; ++i) { frame(false, 500 + i); }
    Require(timing->GetTimingSnapshot().d3d11Samples == 0 &&
        timing->GetTimingSnapshot().sourcePresentCpu.samples == 0 &&
        timing->GetTimingSnapshot().neuralEarly.samples == 0, "old pending samples cannot repopulate reset window");
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; info->GetMessage(i, nullptr, &size);
        std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        info->GetMessage(i, message, &size);
        Require(message->Severity > D3D11_MESSAGE_SEVERITY_ERROR, message->pDescription);
    }
    std::puts("Source timing PASS: independent copy queries, absent stages, CPU/NR totals, disabled/reset windows, debug layer");
}
