#pragma once

// Explicit hardware case, separate from the normal renderer CTest registry.
// Mirrors the host's five proxy interfaces, cached buffers, shared transport,
// retirement helper, same-HWND reuse and failed-rebuild native recovery.
static void ExercisePresenterRebuild(TheosRenderPipeline::XeFGPresenter& presenter, IDXGIFactory* factory,
    ID3D11Device* d11, ID3D11DeviceContext* c11, ID3D12Device* d12, ID3D12CommandQueue* queue,
    Interop& interop, DXGI_SWAP_CHAIN_DESC desc, const std::filesystem::path& intel, bool tensorSwitch, NvidiaPresenterFixture* nvidia)
{
    using namespace TheosRenderPipeline;
    Inputs input;
    input.Create(interop, d11, desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);
    ComPtr<IDXGISwapChain> retained;
    auto* stable = input.color.texture11.Get();
    constexpr unsigned rounds = 8;
    unsigned generated[rounds]{};
    ULONG stableCount{};
    for (unsigned round = 0; round < rounds; ++round) {
        const bool tensor = tensorSwitch && (round == 4 || round == 6);
        const bool inverted = round == 2;
        Check(presenter.Probe(d12, intel, tensor), "rebuild model admission");
        std::printf("REBUILD round=%u tensorRequest=%u inverted=%u\n", round, tensor, inverted);
        ComPtr<IDXGISwapChain> proxy;
        Check(presenter.Create(d12, queue, factory, desc, inverted, &proxy, presenter.Snapshot().frameId,
            {round != 0, round ? 3u : 1u}, nullptr, retained.Get()), "rebuild create x2 then unlocked x4");
        const auto tensorBefore = presenter.TensorStats();
        if (tensor) {
            Require(presenter.Snapshot().tensorModel, "requested ported model is active");
            const bool debugLayer = GetEnvironmentVariableW(L"TRP_XEFG_NO_DEBUG", nullptr, 0) == 0;
            Require(debugLayer || presenter.Snapshot().tensorActive, "unwrapped 4080 executes tensor helper");
            std::printf("rebuild_model tensorActive=%u debugLayer=%u\n", presenter.Snapshot().tensorActive, debugLayer);
        }
        if (round) { Require(presenter.NativeSwapChain() == retained.Get(), "x2 to x4 keeps exact native HWND owner"); }
        ComPtr<IDXGISwapChain1> p1;
        ComPtr<IDXGISwapChain2> p2;
        ComPtr<IDXGISwapChain3> p3;
        ComPtr<IDXGISwapChain4> p4;
        Check(proxy.As(&p1), "rebuild proxy1"); Check(proxy.As(&p2), "rebuild proxy2");
        Check(proxy.As(&p3), "rebuild proxy3"); (void)proxy.As(&p4);
        std::array<ComPtr<ID3D12Resource>, 2> buffers;
        for (UINT i = 0; i < buffers.size(); ++i) { Check(proxy->GetBuffer(i, IID_PPV_ARGS(&buffers[i])), "rebuild cached proxy buffer"); }
        for (unsigned frame = 0; frame < 28; ++frame) {
            if (frame) { Check(presenter.BeginFrame(), "rebuild begin"); }
            input.Paint(c11, frame);
            Check(interop.CopyInput(input.producerColor.Get(), input.color), "rebuild color");
            Check(interop.CopyInput(input.producerHudless.Get(), input.hudless), "rebuild hudless");
            Check(interop.CopyInput(input.producerDepth.Get(), input.depth), "rebuild depth");
            Check(interop.CopyInput(input.producerMotion.Get(), input.motion), "rebuild motion");
            sl::Constants camera{};
            camera.cameraRight = {1,0,0}; camera.cameraUp = {0,1,0}; camera.cameraFwd = {0,0,-1};
            camera.cameraViewToClip.row[0] = {1,0,0,0}; camera.cameraViewToClip.row[1] = {0,1,0,0};
            camera.cameraViewToClip.row[2] = {0,0,1,0}; camera.cameraViewToClip.row[3] = {0,0,0,1};
            camera.depthInverted = inverted ? sl::eTrue : sl::eFalse;
            camera.reset = frame == 0 ? sl::eTrue : sl::eFalse;
            Check(presenter.Prepare(camera), "rebuild prepare");
            ID3D12GraphicsCommandList* list{};
            Check(interop.SignalD3D11(Work::SwapChain), "rebuild source signal");
            Check(interop.Begin(Work::SwapChain, &list), "rebuild output begin");
            Check(Interop::RecordCopy(list, input.color.texture12.Get(), buffers[p3->GetCurrentBackBufferIndex()].Get()), "rebuild copy");
            Check(presenter.BeforePresent(list, input.motion.texture12.Get(), input.depth.texture12.Get(),
                input.hudless.texture12.Get(), nullptr, frame >= 4, true, 60, round ? 3u : 1u), "rebuild tags");
            Check(interop.Submit(Work::SwapChain), "rebuild submit");
            Check(presenter.FinalizePresent(), "rebuild finalize");
            const auto presented = proxy->Present(0, 0);
            Check(presented, "rebuild Present");
            Check(presenter.AfterPresent(presented), "rebuild complete");
            if (presenter.Snapshot().framesPresented > 1) { ++generated[round]; }
            Check(interop.WaitForInputReaders(nullptr, 0), "rebuild post-Present bridge");
            Check(interop.Drain(), "rebuild input retirement");
        }
        Require(presenter.Snapshot().generatedFrames == (round ? 3u : 1u), "actual requested x2/x4 generation");
        Require(generated[round] > 10, "sustained output after every rebuild");
        if (tensor && presenter.Snapshot().tensorActive) { Require(presenter.TensorStats().launches > tensorBefore.launches && presenter.Snapshot().tensorFallbacks == 0,
            "reused-chain tensor model actually dispatches without fallback"); }
        Check(presenter.Disable(), "rebuild disable");
        Check(interop.WaitForInputReaders(nullptr, 0), "rebuild retirement bridge");
        Check(interop.Drain(), "rebuild retire");
        retained = presenter.NativeSwapChain();
        PresenterReleaseProbe probe(retained.Get());
        Check(ReleasePresenterAfterRetirement([&] { buffers = {}; p4.Reset(); p3.Reset(); p2.Reset(); p1.Reset(); proxy.Reset(); },
            [&] { return presenter.Destroy(); }, [&] { c11->ClearState(); c11->Flush(); }, [&] {
                auto hr = interop.WaitForInputReaders(nullptr, 0); return FAILED(hr) ? hr : interop.Drain();
            }), "rebuild release retired proxy and SDK");
        probe.Release([](UINT i, void*, HRESULT hr, ULONG n) {
            std::printf("rebuild_probe buffer=%u hr=%08X remaining=%lu reuseNative=1\n", i, unsigned(hr), n);
        }, [&](void*, ULONG n) {
            std::printf("rebuild_probe round=%u chainRemaining=%lu reuseNative=1\n", round, n);
            if (!round) { stableCount = n; }
            Require(n == stableCount, "no native-reference growth across repeated rebuilds/model/depth changes");
        });
        Require(!presenter.HasContext() && !presenter.HasLatencyContext() && !presenter.NativeSwapChain(), "all old SDK/capture owners retired");
        Require(!(presenter.PacingStats().flags & XeFGPacing::Registered), "old tensor pacing owner retired");
        Require(stable == input.color.texture11.Get(), "rebuild preserves shared producer identity");
    }
    // Full XeFG release before a different provider must relinquish HWND.
    PresenterReleaseProbe providerProbe(retained.Get());
    retained.Reset();
    providerProbe.Release([](UINT, void*, HRESULT hr, ULONG) { Check(hr, "provider buffer probe"); },
        [](void*, ULONG n) { Require(n == 0, "provider change fully releases actual native chain"); std::printf("provider_probe chainRemaining=%lu\n", n); });
    c11->ClearState(); c11->Flush();
    Check(interop.WaitForInputReaders(nullptr,0), "provider post-release bridge"); Check(interop.Drain(), "provider post-release drain");
    ComPtr<IDXGISwapChain> other;
    Check((nvidia ? nvidia->proxyFactory.Get() : factory)->CreateSwapChain(queue, &desc, &other), "other provider acquires HWND");
    Check(other->Present(0,0), "other provider first Present");
    if (nvidia) { nvidia->Begin(presenter.Snapshot().frameId); nvidia->BeforePresent(); }
    Check(other->Present(0,0), "other provider Present");
    if (nvidia) { nvidia->AfterPresent(S_OK); }
    Check(interop.WaitForInputReaders(nullptr,0), "other provider bridge"); Check(interop.Drain(), "other provider drain");
    other.Reset();
    if (nvidia) { nvidia->ReleaseRetiredResources(); }
    c11->ClearState(); c11->Flush();
    Check(interop.WaitForInputReaders(nullptr,0), "other provider release bridge"); Check(interop.Drain(), "other provider release drain");
    ComPtr<IDXGISwapChain> fresh;
    Check(presenter.Probe(d12, intel, false), "return to stock SDK admission");
    Check(presenter.Create(d12, queue, factory, desc, false, &fresh, presenter.Snapshot().frameId, {true,3}), "XeFG reacquires HWND after full provider release");
    retained = presenter.NativeSwapChain(); fresh.Reset();
    Check(presenter.Destroy(), "release fresh SDK before refusal test");
    // Inject a refused new SDK, then drive the production recovery policy. The
    // retained native object must display without CreateSwapChain on the HWND.
    PresenterReplacement recovery; recovery.Start();
    unsigned refused{};
    ComPtr<IDXGISwapChain> partial;
    ComPtr<IDXGISwapChain> fallback;
    while (recovery.Pending()) {
        Check(recovery.Advance([&](bool native) {
            if (!native) {
                ++refused;
                // Fail after the real SDK has acquired native/queue ownership.
                // This verifies cleanup of partial publication before retry.
                Check(presenter.Create(d12, queue, factory, desc, false, &partial, presenter.Snapshot().frameId,
                    {true,3}, nullptr, retained.Get()), "injected partial SDK acquisition");
                return E_FAIL;
            }
            const auto hr = PresenterFactory::NativeFallbackAfterRetirement(retained.Get(), queue);
            if (SUCCEEDED(hr)) { fallback = retained; }
            return hr;
        }, [&] {
            return ReleasePresenterAfterRetirement([&] { partial.Reset(); }, [&] { return presenter.Destroy(); },
                [&] { c11->ClearState(); c11->Flush(); }, [&] {
                    const auto hr = interop.WaitForInputReaders(nullptr,0); return FAILED(hr) ? hr : interop.Drain();
                });
        }), "rebuild injected refusal recovery");
    }
    Require(recovery.Native() && refused == PresenterReplacement::MaxAttempts && fallback.Get() == retained.Get(), "refused SDK keeps native display owner");
    Check(fallback->Present(0,0), "rebuild fallback actually presents");
    Check(interop.WaitForInputReaders(nullptr,0), "fallback retirement bridge"); Check(interop.Drain(), "fallback retirement");
    PresenterReleaseProbe finalProbe(retained.Get());
    fallback.Reset(); retained.Reset();
    finalProbe.Release([](UINT i, void*, HRESULT hr, ULONG n) {
        std::printf("final_probe buffer=%u hr=%08X remaining=%lu\n",i,unsigned(hr),n);
    }, [](void*, ULONG n) { Require(n == 0, "final native owner released"); std::printf("final_probe chainRemaining=%lu\n",n); });
    c11->ClearState(); c11->Flush();
    Check(interop.WaitForInputReaders(nullptr,0), "final probe bridge"); Check(interop.Drain(), "final probe retirement");
    Require(generated[0] > 10 && generated[1] > 10, "sustained x2 and x4 output");
    ComPtr<ID3D12InfoQueue> messages;
    if (SUCCEEDED(d12->QueryInterface(IID_PPV_ARGS(&messages)))) {
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{}; messages->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            messages->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "%s\n", message->pDescription); Require(false, "rebuild D3D12 debug errors");
            }
        }
    }
    std::printf("PASS eight same-HWND lifetimes; x2-to-x4 generated=%u/%u; injected SDK refusal restores retained native; final native references=0; physicalCadenceVerified=false\n", generated[0], generated[1]);
}
