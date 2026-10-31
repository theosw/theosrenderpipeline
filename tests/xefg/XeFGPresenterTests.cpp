#include "XeFGTestSupport.h"
#include "FrameGen/XeFGPresenter.h"
#include "FrameGen/PresentationVSync.h"
#include "NvidiaPresenterFixture.h"
#include "FrameGen/PipelineCache.h"
#include "FrameGen/PresenterReleaseProbe.h"
#include "FrameGen/PresenterReplacement.h"
#include "PluginPaths.h"
#include <dbghelp.h>
#include <chrono>
#include <algorithm>
#include "PresenterRebuildCases.h"
#include "AlternatingLoad.h"
#include "FrameGen/XeFGModelBeat.h"
#include <thread>

static void PrintTiming(const char* label, std::vector<double> values)
{
    Require(!values.empty(), "XeFG timing samples");
    std::sort(values.begin(), values.end());
    std::printf("XEFG_TIMING %s samples=%zu p50=%.6f p95=%.6f ms (fixture CPU boundary, not GPU model time)\n", label,
        values.size(), values[(values.size() - 1) / 2], values[std::size_t((values.size() - 1) * .95)]);
}

// Production owner on hardware. Plain DXGI presenters check HWND retirement
// before/after Intel; actual Streamline replacement remains a separate check.
int wmain(int argc, wchar_t** argv)
{
    // Keep an unhandled-exception dump from non-debugger runs: debugger timing
    // can hide SDK lifecycle faults. This fixture is run from a private output directory.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exception) -> LONG {
        auto file = CreateFileW(
            L"xefg-presenter-crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
            MiniDumpWriteDump(
                GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &info, nullptr, nullptr);
            CloseHandle(file);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Require(argc == 2 || argc == 3, "Intel runtime directory, optional NVIDIA runtime directory");
    const bool profile = GetEnvironmentVariableW(L"TRP_XEFG_PROFILE", nullptr, 0) != 0;
    wchar_t beatText[32]{};
    const bool beatTest = GetEnvironmentVariableW(L"TRP_XEFG_BEAT_TEST", beatText, 32) != 0;
    const unsigned beatMode = beatTest ? std::wcstoul(beatText, nullptr, 10) : 0;
    Require(beatMode <= 3, "beat mode 0=drain, 1=GPU wait, 2=queued beat, 3=drain beat");
    const bool tensorRequested = GetEnvironmentVariableW(L"TRP_XEFG_TENSOR_TEST", nullptr, 0) != 0;
    const bool debugEnabled = (!tensorRequested || GetEnvironmentVariableW(L"TRP_XEFG_REBUILD_TEST", nullptr, 0)) && GetEnvironmentVariableW(L"TRP_XEFG_NO_DEBUG", nullptr, 0) == 0;
    const bool fullscreen = GetEnvironmentVariableW(L"TRP_XEFG_METER_FULLSCREEN", nullptr, 0) != 0;
    MONITORINFO monitor{sizeof(monitor)};
    Require(GetMonitorInfoW(MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY), &monitor) != 0, "primary monitor");
    const unsigned width = fullscreen ? monitor.rcMonitor.right-monitor.rcMonitor.left : profile ? 5120 : 640;
    const unsigned height = fullscreen ? monitor.rcMonitor.bottom-monitor.rcMonitor.top : profile ? 1440 : 360;
    std::vector<double> presentTimes, retirementTimes, sleepTimes;
    ComPtr<ID3D12Debug> debug;
    if (debugEnabled) {
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug");
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Check(factory->EnumAdapters1(0, &adapter), "adapter");
    DXGI_ADAPTER_DESC1 adapterDescription{};
    Check(adapter->GetDesc1(&adapterDescription), "adapter identity");
    std::printf("BEAT_ADAPTER vendor=%04X device=%04X flags=%u\n", adapterDescription.VendorId, adapterDescription.DeviceId, adapterDescription.Flags);
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> c11;
    ComPtr<ID3D12Device> d12;
    Check(D3D11CreateDevice(
              adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &c11),
        "D3D11");
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12)), "D3D12");
    AlternatingLoad load;
    if (beatTest) {
        const double ms = GetEnvironmentVariableW(L"TRP_XEFG_BEAT_GPU_MS", beatText, 32) ? std::wcstod(beatText, nullptr) : 33;
        Require(ms > 0 && ms <= 50, "short bounded load");
        load.Initialize(d11.Get(), c11.Get(), ms);
    }
    ComPtr<ID3D12InfoQueue> messages;
    if (debugEnabled) Check(d12.As(&messages), "debug messages");
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue;
    Check(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Interop interop;
    Check(interop.Initialize(d11.Get(), d12.Get(), queue.Get()), "shared owner");
    std::unique_ptr<NvidiaPresenterFixture> nvidia;
    if (argc == 3)
        nvidia = std::make_unique<NvidiaPresenterFixture>(
            std::filesystem::absolute(argv[2]), d12.Get(), factory.Get(), interop);
    auto* presentingFactory = nvidia ? nvidia->proxyFactory.Get() : static_cast<IDXGIFactory*>(factory.Get());
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TRPXeFGPresenterTests";
    Require(RegisterClassW(&wc) != 0, "register window");
    HWND window = CreateWindowW(wc.lpszClassName, L"TRP production XeFG presenter test", fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 440, nullptr, nullptr, wc.hInstance, nullptr);
    Require(window != nullptr, "window");
    ShowWindow(window, fullscreen ? SW_SHOW : SW_SHOWNOACTIVATE);
    SetWindowPos(window, HWND_TOPMOST, fullscreen ? monitor.rcMonitor.left : 20, fullscreen ? monitor.rcMonitor.top : 20,
        fullscreen ? width : 700, fullscreen ? height : 440, fullscreen ? SWP_SHOWWINDOW : SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (fullscreen) SetForegroundWindow(window);
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = width;
    desc.BufferDesc.Height = height;
    // HDR10 layers: AUTO must also cover 2-bit-alpha UI and frames without a UI layer.
    const bool hdr10 = GetEnvironmentVariableW(L"TRP_XEFG_HDR10", nullptr, 0) != 0;
    const DXGI_FORMAT colorFormat = hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.Format = colorFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    TheosRenderPipeline::XeFGPresenter presenter;
    presenter.SetLogger([](const char* text) { std::printf("[Owner] %s\n", text); });
    Check(presenter.Probe(d12.Get(), std::filesystem::absolute(argv[1]), tensorRequested), "production admission");
    if (GetEnvironmentVariableW(L"TRP_XEFG_REBUILD_TEST", nullptr, 0)) {
        ExercisePresenterRebuild(presenter, factory.Get(), d11.Get(), c11.Get(), d12.Get(), queue.Get(), interop, desc, std::filesystem::absolute(argv[1]), tensorRequested, nvidia.get());
        DestroyWindow(window); UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }
    const std::string cacheName = presenter.PipelineCacheName();
    const auto runtimeDirectory = TheosRenderPipeline::PluginPaths::ModulePath(presenter.RuntimeModule()).parent_path();
    const bool optionalRuntime = TheosRenderPipeline::PluginPaths::EqualPath(runtimeDirectory,
        std::filesystem::absolute(argv[1]) / L"XeFG-Tensor");
    Require(cacheName == (optionalRuntime ? "xefg-tensor-v1" : "xefg"),
        "cache scope follows actual selected runtime before Create populates the snapshot");
    std::printf("pipeline_cache_scope name=%s modelBeforeCreate=%u\n",cacheName.c_str(),presenter.Snapshot().tensorModel);
    const bool meteringOff = GetEnvironmentVariableW(L"TRP_XEFG_METERING_OFF",nullptr,0)!=0;
    const bool meteringToggle = GetEnvironmentVariableW(L"TRP_XEFG_METERING_TOGGLE",nullptr,0)!=0;
    const bool vsyncPolicy = GetEnvironmentVariableW(L"TRP_XEFG_VSYNC_POLICY",nullptr,0)!=0;
    const bool expectedDriverRefusal = GetEnvironmentVariableW(L"TRP_XEFG_EXPECT_METER_REFUSAL",nullptr,0)!=0;
    const bool expectedAllRefusal = GetEnvironmentVariableW(L"TRP_XEFG_EXPECT_METER_UNSUPPORTED",nullptr,0)!=0;
    wchar_t updateFailure[8]{};
    const bool expectedUpdateFailure = GetEnvironmentVariableW(L"TRP_XEFG_TEST_METER_UPDATE_FAILURE",updateFailure,8)!=0;
    const bool pacingException = expectedUpdateFailure && (updateFailure[0] == L'7' || updateFailure[0] == L'8');
    const bool unsafeUpdateFailure = expectedUpdateFailure && (updateFailure[0] == L'3' || updateFailure[0] == L'5' || updateFailure[0] == L'8');
    const bool expectedUnsafeMetering = unsafeUpdateFailure || GetEnvironmentVariableW(L"TRP_XEFG_EXPECT_METER_UNSAFE",nullptr,0)!=0;
    presenter.SetFlipMetering(!meteringOff);
    Inputs input;
    input.Create(interop, d11.Get(), width, height, colorFormat);
    auto* stable = input.color.texture11.Get();
    unsigned generated = 0, uiTagged = 0, generatedWithUI = 0, generatedWithoutUI = 0, frameTimeSent = 0,
             frameTimeSkipped = 0;
    const bool experimental = GetEnvironmentVariableW(L"TRP_XEFG_MFG_TEST", nullptr, 0) != 0;
    const bool expectRefusal = GetEnvironmentVariableW(L"TRP_XEFG_EXPECT_REFUSAL", nullptr, 0) != 0;
    Require(!expectRefusal || experimental, "refusal mode requires experimental request");
    constexpr unsigned maximum = TheosRenderPipeline::XeFGMaxGeneratedFrames;
    std::array<unsigned, maximum + 1> generatedCounts{};
    constexpr unsigned frameCount = 120;
    TheosRenderPipeline::PipelineCache pipelines;
    pipelines.Open(d12.Get(), {}); // Exercise cached loads on subsequent presenters.
    const bool nativeOnly = GetEnvironmentVariableW(L"TRP_XEFG_NATIVE_ONLY", nullptr, 0) != 0;
    const bool releaseNvidia = GetEnvironmentVariableW(L"TRP_XEFG_RELEASE_NV", nullptr, 0) != 0;
    wchar_t cyclesText[8]{};
    unsigned cycles = experimental ? 3u : 2u;
    if (GetEnvironmentVariableW(L"TRP_XEFG_CYCLES", cyclesText, 8)) {
        cycles = static_cast<unsigned>(std::wcstoul(cyclesText, nullptr, 10));
        Require(cycles >= 2 && cycles <= 8, "bounded lifecycle cycle count");
    }
    // Production Lab A/B: queue a D3D11 GPU wait for the input copies instead of CPU drains.
    const bool gpuInputWait = GetEnvironmentVariableW(L"TRP_XEFG_GPU_INPUT_WAIT", nullptr, 0) != 0 || (beatTest && beatMode == 1);
    // Pacing experiment (TRP_XEFG_STALL_TEST, round 0, prints Present times; not asserted):
    // 1 = six 500 ms frames while generating (9-14); 2 = the same during passthrough
    // before generation (2-7); 3 = like 1, then passthrough for frames 15-24.
    // Intel's pacing remembers 1 for ~25 slow Presents, ignores 2, and keeps 1's
    // memory through 3's passthrough: see XeFGSettleGate.
    wchar_t stallText[4]{};
    const unsigned stallMode = GetEnvironmentVariableW(L"TRP_XEFG_STALL_TEST", stallText, 4) ? stallText[0] - L'0' : 0u;
    std::printf("owner cycles=%u releaseRetiredNvidia=%u experimental=%u inputReuse=%s\n", cycles, releaseNvidia,
        experimental, gpuInputWait ? "gpu-fence" : "cpu-drain");
    for (unsigned round = 0; round < cycles; ++round) {
        TheosRenderPipeline::XeFGModelBeat beat;
        std::chrono::steady_clock::time_point previousPresent{};
        if (expectedUpdateFailure) SetEnvironmentVariableW(L"TRP_XEFG_PACING_FAULT",nullptr);
        ComPtr<IDXGISwapChain> native;
        Check(presentingFactory->CreateSwapChain(queue.Get(), &desc, &native), "native before Intel");
        if (nvidia) {
            nvidia->Begin(presenter.Snapshot().frameId);
            nvidia->BeforePresent();
        }
        Check(native->Present(0, 0), "native passthrough");
        if (nvidia) nvidia->AfterPresent(S_OK);
        Check(interop.WaitForInputReaders(nullptr, 0), "native retirement bridge");
        Check(interop.Drain(), "native queue retirement");
        native.Reset();
        if (nvidia && releaseNvidia) { nvidia->ReleaseRetiredResources(); }
        if (!nativeOnly) {
            ComPtr<IDXGISwapChain> proxy;
            Check(presenter.Create(d12.Get(), queue.Get(), factory.Get(), desc, round == 1, &proxy,
                      nvidia ? nvidia->session.Snapshot().frameIndex : 0, {experimental && round < 2, maximum}, pipelines.Library()),
                "production Intel presenter");
            ComPtr<IDXGISwapChain3> proxy3;
            Check(proxy.As(&proxy3), "proxy3");
            ComPtr<IDXGISwapChain1> proxy1;
            Check(proxy.As(&proxy1), "proxy1");
            const auto tensorBefore = presenter.TensorStats();
            Require(presenter.PipelineCacheName() == cacheName, "Create keeps the selected bundle's cache scope");
            unsigned autoFollowRefusalFrames{};
            for (unsigned frame = 0; frame < frameCount; ++frame) {
                if (expectedUpdateFailure && frame == 24) {
                    const auto state = presenter.PacingStats();
                    Require(state.flipCalls > 0 && state.driverStatus == 0,
                        "driver configuration succeeds before injected update refusal");
                    SetEnvironmentVariableW(L"TRP_XEFG_PACING_FAULT",updateFailure);
                    std::printf("metering_update_failure round=%u frame=%u mode=%ls priorCalls=%llu\n",round,frame,
                        updateFailure,state.flipCalls);
                }
                const auto vsyncMode = frame >= 32 && frame < 64 ? VSyncMode::On : VSyncMode::Auto;
                if (vsyncPolicy) presenter.SetFlipMetering(vsyncMode != VSyncMode::On);
                if (meteringToggle && (frame==32 || frame==64)) {
                    presenter.SetFlipMetering(frame==64);
                    std::printf("metering_test round=%u frame=%u enabled=%u\n",round,frame,frame==64);
                }
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (!gpuInputWait && !(beatTest && beatMode == 2 && beat.TargetUs() > 0)) Check(interop.Drain(), "producer reuse");
                input.Paint(c11.Get(), frame);
                std::vector<uint16_t> motions(width * height * 2, 0);
                for (size_t i = 0; i < motions.size(); i += 2)
                    motions[i] = DirectX::PackedVector::XMConvertFloatToHalf(-2.0f / width);
                c11->UpdateSubresource(input.producerMotion.Get(), 0, nullptr, motions.data(), width * 4, 0);
                Check(interop.CopyInput(input.producerColor.Get(), input.color), "color");
                Check(interop.CopyInput(input.producerHudless.Get(), input.hudless), "hudless");
                Check(interop.CopyInput(input.producerMotion.Get(), input.motion), "motion");
                Check(interop.CopyInput(input.producerDepth.Get(), input.depth), "depth");
                Check(interop.CopyInput(input.producerUI.Get(), input.ui), "UI layer");
                sl::Constants camera{};
                camera.cameraRight = {1, 0, 0};
                camera.cameraUp = {0, 1, 0};
                camera.cameraFwd = {0, 0, -1};
                camera.cameraPos = {-4.0f * frame / width, 0, 0};
                camera.cameraViewToClip.row[0] = {1, 0, 0, 0};
                camera.cameraViewToClip.row[1] = {0, 1, 0, 0};
                camera.cameraViewToClip.row[2] = {0, 0, 1, 0};
                camera.cameraViewToClip.row[3] = {0, 0, 0, 1};
                camera.jitterOffset = {0, 0};
                camera.depthInverted = round == 1 ? sl::eTrue : sl::eFalse;
                camera.reset = frame == 8 || frame == 45 ? sl::eTrue : sl::eFalse;
                // Round 1 sends the measured frame time; round 0 leaves Intel's estimate.
                if (beatTest) {
                    if (!(frame % 2)) {
                        load.Record(c11.Get());
                        c11->Flush(); // Start the load before the overlapping CPU interval.
                    }
                    // CPU game work overlaps the queued synthetic model.
                    Sleep(14);
                }
                Check(presenter.Prepare(camera, round == 1), "production camera");
                Require(presenter.Snapshot().prepared, "depth convention");
                // Intervals over 250 ms (a slow profile frame, or a multiplier change that
                // rebuilds Intel's shaders) correctly send 0, so require most frames, not all.
                const float sentFrameTime = presenter.Snapshot().frameTimeMs;
                Require(round == 1 && frame >= 1 ? sentFrameTime >= 0 && sentFrameTime <= 250 : sentFrameTime == 0,
                    "frame time sent only when requested and measured");
                if (round == 1 && frame >= 1) ++(sentFrameTime > 0 ? frameTimeSent : frameTimeSkipped);
                // Round 0 exercises both Intel debug views live; recreation starts with them off.
                const bool onlyGenerated = round == 0 && frame >= 70 && frame < 76,
                           tagGenerated = round == 0 && frame >= 60 && frame < 76;
                if (round == 1 && frame == 0)
                    Require(!presenter.Snapshot().onlyGenerated && !presenter.Snapshot().tagGenerated,
                        "debug views reset with the context");
                Check(presenter.SetDebugView(onlyGenerated, tagGenerated), "debug view");
                Require(presenter.Snapshot().onlyGenerated == onlyGenerated &&
                        presenter.Snapshot().tagGenerated == tagGenerated,
                    "debug view state");
                // Round 0 changes Intel's scene-change sensitivity live; a new context
                // has Intel's default until the threshold is sent again.
                if (round == 1 && frame == 0)
                    Require(presenter.Snapshot().sceneChangeThreshold < 0, "scene-change threshold resets with the context");
                const float sceneChange = round == 0 && frame >= 50 ? 0.25f : TheosRenderPipeline::XeFGDefaultSceneChangeThreshold;
                presenter.SetSceneChangeThreshold(sceneChange);
                Require(presenter.Snapshot().sceneChangeThreshold == sceneChange, "Intel accepts the scene-change threshold");
                ID3D12GraphicsCommandList* list{};
                Check(interop.SignalD3D11(Work::SwapChain), "signal source");
                Check(interop.Begin(Work::SwapChain, &list), "begin output");
                ComPtr<ID3D12Resource> back;
                Check(proxy->GetBuffer(proxy3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)), "backbuffer");
                Check(Interop::RecordCopy(list, input.color.texture12.Get(), back.Get()), "source copy");
                const bool holdAfterStall = stallMode == 3 && round == 0 && frame >= 15 && frame < 25;
                const bool enable = frame >= 8 && frame < 112 && !holdAfterStall;
                // Round 0 tags the UI layer, then offers a mismatched layer that must be
                // rejected; later rounds provide none, leaving AUTO to extract from the frame.
                auto* ui = round == 0 ? (frame < 45 ? input.ui.texture12.Get() : input.depth.texture12.Get()) : nullptr;
                const bool uiExpected = enable && round == 0 && frame < 45;
                // Round 0 also drops back to x2 and up to x4 on the same unlocked presenter,
                // as the backend does after the first unlock instead of recreating it.
                const unsigned rising = std::min(1u + frame / 18u, maximum);
                const unsigned count = !experimental ? 1u : round == 0 && frame >= 80 ? (frame < 96 ? 1u : 3u) : rising;
                Check(presenter.BeforePresent(list, input.motion.texture12.Get(), input.depth.texture12.Get(),
                          input.hudless.texture12.Get(), ui, enable, true, profile || beatTest ? 0 : 60, count),
                    "production preparation");
                Require(presenter.Snapshot().uiTexture == uiExpected,
                    "UI layer tagged only when it matches the HUD-less layer");
                if (uiExpected) ++uiTagged;
                Check(interop.Submit(Work::SwapChain), "submit SDK copies");
                double holdMs{};
                if (beatTest && (beatMode == 2 || beatMode == 3) && enable) {
                    const auto ready = std::chrono::steady_clock::now();
                    const auto d = beat.Plan(std::chrono::duration<double, std::micro>(ready.time_since_epoch()).count(),
                        frame % 2 ? TheosRenderPipeline::NRModelActivity::Skipped : TheosRenderPipeline::NRModelActivity::Ran);
                    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::micro>(d.deadlineUs))));
                    const auto released = std::chrono::steady_clock::now();
                    holdMs = std::chrono::duration<double, std::milli>(released - ready).count();
                    beat.Released(std::chrono::duration<double, std::micro>(released.time_since_epoch()).count());
                } else if (beatTest && !enable) beat.Reset();
                back.Reset();
                Check(presenter.FinalizePresent(), "markers after queue submission");
                if (round == 0 && ((stallMode == 1 || stallMode == 3) ? frame >= 9 && frame < 15 : stallMode == 2 && frame >= 2 && frame < 8))
                    Sleep(500);
                const auto presentStart = std::chrono::steady_clock::now();
                const auto beforePacing = presenter.PacingStats();
                const auto pacingBeforePresent = beforePacing.flags;
                const auto presentation = ResolveVSync({vsyncMode,TheosRenderPipeline::XeFGPacing::CanMeter(pacingBeforePresent),true},1,0,true);
                if (vsyncPolicy && frame == 0) {
                    Require((pacingBeforePresent & TheosRenderPipeline::XeFGPacing::Unavailable)
                        && presentation.sync == 1 && !(presentation.flags & DXGI_PRESENT_ALLOW_TEARING),
                        "Auto follows caller before the first capability probe");
                }
                DXGI_PRESENT_PARAMETERS parameters{};
                const auto result = vsyncPolicy ? (frame % 2 ? proxy1->Present1(presentation.sync,presentation.flags,&parameters) :
                    proxy->Present(presentation.sync,presentation.flags)) : proxy->Present(0, 0);
                const auto presentEnd = std::chrono::steady_clock::now();
                if (stallMode && round == 0 && frame < 60)
                    std::printf("stall-test frame=%u present=%.1f ms\n", frame,
                        std::chrono::duration<double, std::milli>(presentEnd - presentStart).count());
                Check(result, "Intel present");
                Require(result == S_OK, "visible present");
                const auto completedPresent=presenter.AfterPresent(result);
                if (vsyncPolicy && frame == 0) {
                    const auto state=presenter.PacingStats();
                    Require(state.flipCalls == beforePacing.flipCalls + 1,
                        "off-only capability probe runs on first passthrough Present");
                    Require(bool(state.flags & TheosRenderPipeline::XeFGPacing::Unavailable) == expectedAllRefusal,
                        "first passthrough publishes capability admission or refusal");
                    std::printf("metering_first_present round=%u sync=%u flags=0x%X pacingFlags=%u calls=%llu\n",
                        round,presentation.sync,presentation.flags,state.flags,state.flipCalls);
                }
                if (vsyncPolicy && vsyncMode == VSyncMode::On)
                    Require(!(presenter.PacingStats().flags & TheosRenderPipeline::XeFGPacing::Metered),"VSync On disables metering");
                if (vsyncPolicy && (frame==31 || frame==32 || frame==63 || frame==64)) {
                    const auto state=presenter.PacingStats();
                    std::printf("vsync_policy round=%u frame=%u mode=%s incoming=1 submitted=%u flags=0x%X pacingFlags=%u metered=%llu\n",
                        round,frame,VSyncName(vsyncMode),presentation.sync,presentation.flags,state.flags,state.meteredCalls);
                }
                if (expectedUnsafeMetering && FAILED(completedPresent)) {
                    const auto failed=presenter.PacingStats();
                    Require((failed.flags&TheosRenderPipeline::XeFGPacing::Unsafe)&&failed.driverFailures>0,"failed driver off call retains uncertain state");
                    Check(interop.Drain(),"host retirement after injected metering fault");
                    proxy1.Reset();proxy3.Reset();proxy.Reset();
                    Require(FAILED(presenter.Destroy()),"uncertain driver configuration prevents successful provider retirement");
                    Require(presenter.PacingStats().flags&TheosRenderPipeline::XeFGPacing::Registered,"unsafe pacing owner is retained");
                    Require(presenter.TensorStats().flags&TheosRenderPipeline::XeFGTensor::ModelReady,"model resources retained after metering retirement fault");
                    std::puts("PASS injected driver-off failure: owner/model retained, replacement blocked; restart required");
                    return 0;
                }
                Check(completedPresent, "production completion");
                if (vsyncPolicy && vsyncMode == VSyncMode::Auto &&
                    (pacingBeforePresent & TheosRenderPipeline::XeFGPacing::Unavailable)) {
                    Require(presentation.sync == 1 && !(presentation.flags & DXGI_PRESENT_ALLOW_TEARING),
                        "Auto follows caller sync without tearing once metering is unavailable");
                    ++autoFollowRefusalFrames;
                }
                if (expectedUpdateFailure && frame == 48) {
                    const auto state = presenter.PacingStats();
                    Require((pacingException || state.driverFailures > 0) && state.driverStatus == 0,
                        "failed updates followed by acknowledged driver off");
                    Require(state.flags & TheosRenderPipeline::XeFGPacing::Unavailable,
                        "disabled/refused pacing becomes unavailable to Auto");
                    Require(!(state.flags & (TheosRenderPipeline::XeFGPacing::Metered | TheosRenderPipeline::XeFGPacing::Unsafe)),
                        "software pacing after failed updates is cleanly unmetered");
                    std::printf("metering_update_recovered round=%u calls=%llu failures=%llu status=%d\n",
                        round,state.flipCalls,state.driverFailures,state.driverStatus);
                    SetEnvironmentVariableW(L"TRP_XEFG_PACING_FAULT",nullptr);
                }
                const auto retirementStart = std::chrono::steady_clock::now();
                const bool queued = gpuInputWait || (beatTest && beatMode == 2 && beat.TargetUs() > 0);
                Check(queued ? interop.WaitD3D11(Work::SwapChain) : interop.Drain(),
                    gpuInputWait ? "ONLY_NOW input reuse GPU wait" : "retire ONLY_NOW copies");
                const auto retirementEnd = std::chrono::steady_clock::now();
                if (beatTest) {
                    const auto cadence = previousPresent == std::chrono::steady_clock::time_point{} ? 0. :
                        std::chrono::duration<double, std::milli>(presentStart - previousPresent).count();
                    previousPresent = presentStart;
                    std::printf("BEAT_FRAME round=%u frame=%u model=%s queued=%u cadenceMs=%.3f presentMs=%.3f reuseMs=%.3f beginSleepMs=%.3f holdMs=%.3f targetMs=%.3f outputs=%u batchStart=unavailable outputSpacing=unavailable\n",
                        round, frame, frame % 2 ? "skipped" : "ran", queued, cadence,
                        std::chrono::duration<double, std::milli>(presentEnd - presentStart).count(),
                        std::chrono::duration<double, std::milli>(retirementEnd - retirementStart).count(), presenter.Snapshot().sleepMs, holdMs,
                        beat.TargetUs()/1000, presenter.Snapshot().framesPresented);
                }
                if (profile && enable && frame >= 24) {
                    presentTimes.push_back(
                        std::chrono::duration<double, std::milli>(presentEnd - presentStart).count());
                    retirementTimes.push_back(
                        std::chrono::duration<double, std::milli>(retirementEnd - retirementStart).count());
                }
                const auto& state = presenter.Snapshot();
                Require(state.framesPresented >= 1 && state.framesPresented <= state.generatedFrames + 1,
                    "selected count bounds");
                if (!enable) Require(state.framesPresented == 1, "off passthrough");
                Require(state.frameLimitUs == (profile || beatTest ? 0 : 16667), "output cap remains independent of ratio");
                Require(state.maxGeneratedFrames == (state.tensorModel || (experimental && !expectRefusal && round < 2) ? maximum : 1u),
                    "opt-in capacity, including disable after resident unlock");
                if (expectRefusal)
                    Require(!state.unlockReady && state.generatedFrames == 1, "refused unlock retains official x2");
                if (experimental && !expectRefusal && round == 0 && (frame == 90 || frame == 105))
                    Require(state.maxGeneratedFrames == maximum && state.generatedFrames == (frame == 90 ? 1u : 3u),
                        "unlocked presenter returns to x2 and back up live, keeping its capacity");
                if (enable && state.framesPresented == state.generatedFrames + 1) {
                    ++generated;
                    ++generatedCounts[state.generatedFrames];
                    ++(state.uiTexture ? generatedWithUI : generatedWithoutUI);
                }
                if (frame % 30 == 0)
                    std::printf("production round=%u requested=%u actual=%u outputs=%u capacity=%u optIn=%u\n", round,
                        count + 1, state.generatedFrames + 1, state.framesPresented, state.maxGeneratedFrames + 1,
                        state.experimentalMFG);
                if (frame + 1 != frameCount) {
                    const auto start = std::chrono::steady_clock::now();
                    Check(presenter.BeginFrame(), "next frame");
                    if (profile && enable && frame >= 24)
                        sleepTimes.push_back(
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                                .count());
                }
            }
            Check(presenter.Disable(), "disable");
            const auto completed = presenter.Snapshot();
            const auto tensorAfter = presenter.TensorStats();
            const auto paced = presenter.PacingStats();
            if (vsyncPolicy && (expectedDriverRefusal || expectedAllRefusal)) {
                Require(autoFollowRefusalFrames > 0, "actual metering refusal reaches Auto follow-caller policy");
                std::printf("auto_refusal_follow round=%u frames=%u\n",round,autoFollowRefusalFrames);
            }
            std::printf("pacing_complete round=%u flags=%u deadlines=%llu extra=%llu calls=%llu metered=%llu failures=%llu feedback=%u status=%d\n",
                round,paced.flags,paced.deadlines,paced.extraPaces,paced.flipCalls,paced.meteredCalls,paced.driverFailures,paced.feedback,paced.driverStatus);
            if (expectedAllRefusal) {
                Require((paced.flags & TheosRenderPipeline::XeFGPacing::Unavailable) &&
                    !(paced.flags & TheosRenderPipeline::XeFGPacing::Unsafe) && paced.flipCalls == round + 1,
                    "unsupported driver receives exactly one off-only probe per owner, no enable attempts or unsafe state");
            }
            if (GetEnvironmentVariableW(L"TRP_XEFG_PACING_TEST",nullptr,0)) {
                Require((paced.flags & TheosRenderPipeline::XeFGPacing::Registered)!=0,"private pacing context registration");
                Require(paced.deadlines>0,"actual SDK deadline callbacks");
                // When Auto follows sync1 after capability refusal, the SDK's
                // synchronous Present path supplies the waits. Extra async
                // output waits are not expected on that path.
                if (!(expectedAllRefusal && vsyncPolicy))
                    Require(paced.extraPaces>0,"intermediate output pacing on the async path");
                Require(meteringOff ? paced.flipCalls==0 : paced.flipCalls>0,"driver metering requests observed");
                Require(expectedAllRefusal ? paced.driverFailures>0&&paced.meteredCalls==0 :
                    expectedUpdateFailure ? (pacingException || paced.driverFailures>0)&&paced.driverStatus==0 :
                    expectedDriverRefusal ? paced.driverFailures>0&&paced.meteredCalls==0 : paced.driverFailures==0,
                    "driver refusal uses software pacing; normal configuration stays clean");
                if (fullscreen&&!meteringOff&&!expectedDriverRefusal&&!expectedAllRefusal) Require(paced.meteredCalls>0,"independent-flip driver metering actually accepted");
            }
            std::printf("tensor_complete round=%u model=%u active=%u launches=%llu fallbacks=%llu cachedPSOs=%llu libraryBytes=%zu\n", round,
                completed.tensorModel, completed.tensorActive, completed.tensorLaunches, completed.tensorFallbacks,
                tensorAfter.cachedPSOs, pipelines.Library() ? pipelines.Library()->GetSerializedSize() : 0);
            if (completed.tensorActive) Require(completed.tensorLaunches > tensorBefore.launches && completed.tensorFallbacks == 0,
                "actual tensor dispatch and no unexpected fallback");
            Check(interop.Drain(), "retire shared resources");
            Require(presenter.NativeSwapChain() != nullptr, "actual DXGI chain captured beneath Intel proxy");
            TheosRenderPipeline::PresenterReleaseProbe releaseProbe(presenter.NativeSwapChain());
            proxy1.Reset();
            proxy3.Reset();
            proxy.Reset();
            Check(presenter.Destroy(), "SDK private retirement");
            releaseProbe.Release([](UINT i, void* identity, HRESULT hr, ULONG remaining) {
                std::printf("release_probe buffer=%u ptr=%p hr=%08X remaining=%lu\n", i, identity, unsigned(hr), remaining);
            }, [](void* identity, ULONG remaining) {
                std::printf("release_probe chain=%p remaining=%lu\n", identity, remaining);
            });
            c11->ClearState(); c11->Flush();
            Check(interop.WaitForInputReaders(nullptr, 0), "post-destruction queue bridge");
            Check(interop.Drain(), "post-destruction drain");
            Require(presenter.PipelineCacheName() == cacheName, "Destroy cannot erase the selected bundle's cache scope");
            Require(!(presenter.PacingStats().flags & TheosRenderPipeline::XeFGPacing::Registered),"no retained pacing owner after successful SDK retirement");
        }
        Require(input.color.texture11.Get() == stable, "stable host resource");
        Check(presentingFactory->CreateSwapChain(queue.Get(), &desc, &native), "native restored on HWND");
        Check(native->Present(0, 0), "establish restored native presentation before latency markers");
        if (nvidia) {
            nvidia->Begin(presenter.Snapshot().frameId);
            nvidia->BeforePresent();
        }
        Check(native->Present(0, 0), "restored present");
        if (nvidia) nvidia->AfterPresent(S_OK);
        Check(interop.WaitForInputReaders(nullptr, 0), "restored native retirement bridge");
        Check(interop.Drain(), "restored native queue retirement");
        native.Reset();
    }
    Require(!expectedUnsafeMetering, "injected driver-off failure must be observed");
    Require(nativeOnly || generated > 100, "sustained generation both depth conventions");
    Require(presenter.Snapshot().uiTexturePresents == uiTagged && (nativeOnly || uiTagged > 0), "UI layer tag count");
    std::printf("frame time sent=%u skipped=%u\n", frameTimeSent, frameTimeSkipped);
    Require(nativeOnly || frameTimeSent > frameTimeSkipped * 4, "measured frame time sent on most frames");
    // AUTO must keep generating on frames without a UI layer, in SDR and HDR10.
    std::printf("generated withUI=%u withoutUI=%u hdr10=%u\n", generatedWithUI, generatedWithoutUI, hdr10);
    Require(nativeOnly || (generatedWithUI > 20 && generatedWithoutUI > 20), "generation with and without a UI layer");
    if (profile) {
        PrintTiming("outer Present", presentTimes);
        PrintTiming("post-Present reuse wait", retirementTimes);
        PrintTiming("BeginFrame / XeLL sleep", sleepTimes);
    }
    if (experimental && !nativeOnly && !expectRefusal)
        for (unsigned count = 1; count <= maximum; ++count)
            Require(generatedCounts[count] > 20, "production live x2 through x6 and disable-to-x2");
    if (expectRefusal && !nativeOnly) {
        Require(generatedCounts[1] > 100, "sustained official x2 after unlock refusal");
        for (unsigned count = 2; count <= maximum; ++count)
            Require(generatedCounts[count] == 0, "refusal never enables experimental counts");
    }
    for (UINT64 n = 0; messages && n < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++n) {
        SIZE_T size{};
        messages->GetMessage(n, nullptr, &size);
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        messages->GetMessage(n, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr, "%s\n", message->pDescription);
            Require(false, "debug errors");
        }
    }
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::printf(
        "PASS mode=%s productionOwner generatedPresents=%u uiTexturePresents=%u stableSharedResources=true physicalCadenceVerified=false\n",
        nativeOnly   ? "NVIDIA-control"
            : nvidia ? "NVIDIA-XeFG-NVIDIA"
                     : "DXGI-XeFG-DXGI",
        generated, uiTagged);
    for (unsigned count = 1; count <= maximum; ++count)
        std::printf("multiplier coverage x%u=%u experimental=%u\n", count + 1, generatedCounts[count], experimental);
}
