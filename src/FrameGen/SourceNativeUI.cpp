#include <PCH.h>
#include "NvidiaHost.h"
#include "LoadingArtwork.h"
#include "RenderPipeline.h"
#include "PerformanceTuning.h"
#include "LoggingPolicy.h"
#include <utility>

namespace
{
    struct InternalOperation
    {
        bool& flag;
        bool previous;
        explicit InternalOperation(bool& value) : flag(value), previous(std::exchange(value, true)) {}
        ~InternalOperation() { flag = previous; }
    };

    bool MainOrLoading()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
    }
}

bool NvidiaHost::CaptureAndComposeDedicatedNativeUI()
{
    // The direct compositor binds graphics state. Keep our routing hooks from
    // treating its fullscreen draw as another native-UI producer.
    InternalOperation internal(sourceUIInternal_);
    return nativeUI_.Compose(context_.Get(), presentation_.Texture());
}

void NvidiaHost::RegisterSourceGameContext(ID3D11DeviceContext* context)
{
    if (!StartupConfigured() || !proxyActive_) { return; }
    const bool registered = nativeUIContexts_.RegisterGameContext(context);
    logger::info("[NativeUIContext] creation result host={} game={} distinct={} registered={}",
        static_cast<void*>(context_.Get()), static_cast<void*>(context), context != context_.Get(), registered);
}

void NvidiaHost::LogSourceContextHook(unsigned hook, ID3D11DeviceContext* context) const
{
    if (!StartupConfigured() || !UpscalerReady() || NativeUIInternalBind() || hook >= 3) { return; }
    // One accepted and one rejected observation per hook, independent of the
    // frame-route log budget. This also reveals a later unexpected wrapper.
    static std::atomic<unsigned> observed{};
    const bool accepted = SourceContext(context);
    const unsigned bit = 1u << (hook * 2 + unsigned(accepted));
    if (observed.load(std::memory_order_relaxed) & bit) { return; }
    if (observed.fetch_or(bit, std::memory_order_relaxed) & bit) { return; }
    static constexpr const char* names[]{"OMSetRenderTargets", "RSSetViewports", "PSSetShaderResources"};
    logger::info("[NativeUIContext] hook={} incoming={} host={} game={} accepted={}", names[hook],
        static_cast<void*>(context), static_cast<void*>(context_.Get()),
        static_cast<void*>(nativeUIContexts_.GameContext()), accepted);
}

bool NvidiaHost::TakeNativeUITraceSlot()
{
    if (!TheosRenderPipeline::Logging::NativeUITraceEnabled()) { return false; }
    // Capture a few frames on each menu transition, plus periodic samples.
    // Every sampled frame has a fixed budget shared by all hook diagnostics.
    static std::uint64_t lastFrame = ~std::uint64_t{}, untilFrame{};
    static unsigned previousMenus = ~0u, slots{};
    auto* ui = RE::UI::GetSingleton();
    const unsigned menus = ui ? unsigned(ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) |
        (unsigned(ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) << 1) : 0;
    if (menus != previousMenus) { previousMenus = menus; untilFrame = presentCount_ + 3; }
    if (lastFrame != presentCount_) { lastFrame = presentCount_; slots = 0; }
    return StartupConfigured() && (presentCount_ < 6 || presentCount_ <= untilFrame || presentCount_ % 600 == 0) && slots++ < 48;
}

void NvidiaHost::LogNativeUIState(const char* stage, bool mainOrLoading)
{
    if (!context_ || !TakeNativeUITraceSlot()) { return; }
    mainOrLoading = MainOrLoading();
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    context_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context_->RSGetViewports(&count, viewports);
    const auto& vp = viewports[0];
    logger::info("[NativeUIRoute] frame={} stage={} selectedBuffer={} menu={} startupReduced={} evaluated={} uiDrawn={} active={} evaluation={} viewportCount={} viewport=({:.1f},{:.1f} {:.1f}x{:.1f} depth={:.2f}..{:.2f})",
        presentCount_, stage, sourceUIBufferIndex_, mainOrLoading,
        nativeUIPass_.Frame().ReduceStartupViewport(mainOrLoading), nativeUIPass_.HasEarlyEvaluation(),
        nativeUIPass_.Frame().UIDrawn(), nativeUIPass_.Active(), evaluationCount_, count,
        vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth);
    auto* upscaler = RenderPipeline::GetSingleton();
    logger::info("[NativeUIRoute] frame={} guides motion={} depth={} input={} reconstructed={} render={}x{} output={}x{} jitter=({:.4f},{:.4f}) pendingHistoryResets={}",
        presentCount_, static_cast<void*>(upscaler->mMotionVectors.mImage), static_cast<void*>(upscaler->mDepthBuffer.mImage),
        static_cast<void*>(gameTargets_.GameFacing()), static_cast<void*>(gameTargets_.UpscaleOutput()), renderWidth_, renderHeight_, outputWidth_, outputHeight_,
        upscaler->mJitterOffsets[0], upscaler->mJitterOffsets[1], upscaler->mPendingHistoryResets);
    const auto describe = [&](const char* role, UINT slot, ID3D11View* view) {
        if (!view) { return; }
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        view->GetResource(&resource);
        if (!resource || FAILED(resource.As(&texture))) { return; }
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        logger::info("[NativeUIRoute] frame={} {}[{}] resource={} extent={}x{} format={} samples={} gameFacing={} presentation={} ui={} enb={}",
            presentCount_, role, slot, static_cast<void*>(texture.Get()), desc.Width, desc.Height,
            static_cast<unsigned>(desc.Format), desc.SampleDesc.Count, texture.Get() == gameTargets_.GameFacing(),
            texture.Get() == presentation_.Texture(), texture.Get() == nativeUI_.RenderTexture(), nativeUIAttachments_.IsENBTarget(view));
    };
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> owned; owned.Attach(rtvs[i]);
        describe("RTV", i, owned.Get());
    }
    describe("DSV", 0, dsv.Get());
}

bool NvidiaHost::PrepareSourceNativeUITargets()
{
    if (!device_ || !context_ || !innerSwapChain_) { return false; }
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain;
    if (FAILED(innerSwapChain_->QueryInterface(IID_PPV_ARGS(&chain)))) { return false; }
    sourceUIBufferIndex_ = chain->GetCurrentBackBufferIndex();
    InternalOperation internal(sourceUIInternal_);
    if (!PrepareNativeUITarget(sourceUIBufferIndex_)) { return false; }
    auto* upscaler = RenderPipeline::GetSingleton();
    const auto hr = nativeUIAttachments_.Ensure(device_.Get(), context_.Get(),
        upscaler->mMotionVectors.mImage, upscaler->mDepthBuffer.mImage, outputWidth_, outputHeight_);
    static HRESULT reportedAttachmentFailure = S_OK;
    if (FAILED(hr) && hr != reportedAttachmentFailure) {
        logger::warn("[NativeUIRoute] native attachments unavailable result=0x{:08X}; keep background eligible for Present", static_cast<unsigned>(hr));
    }
    reportedAttachmentFailure = hr;
    return SUCCEEDED(hr);
}

// This adapter supplies host operations without moving allocation, context
// registration, vendor evaluation or retirement into the frame coordinator.
struct NvidiaHost::SourceFrameOperations
{
    NvidiaHost& host;

    bool ObserveENBBoundary()
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bound;
        host.context_->OMGetRenderTargets(1, &bound, nullptr);
        return host.nativeUIAttachments_.ObserveENBBoundary(bound.Get()) && host.nativeUI_.Dedicated();
    }

    bool PrepareUITargets() { return host.PrepareSourceNativeUITargets(); }
    void ClearAuxiliary() { host.nativeUIAttachments_.ClearAuxiliary(host.context_.Get()); }

    bool EvaluateBackground(bool nativeUI)
    {
        InternalOperation internal(host.sourceUIInternal_);
        return host.EvaluateFrame(host.outerSwapChain_, nativeUI);
    }

    TheosRenderPipeline::NativeUIPass::Target UITarget()
    {
        return {host.NativeUIRenderRTV(), host.NativeUIDepthDSV(), host.outputWidth_, host.outputHeight_, host.nativeUI_.Dedicated()};
    }

    bool PreparePresentTarget()
    {
        Microsoft::WRL::ComPtr<IDXGISwapChain3> chain;
        if (!host.innerSwapChain_ || FAILED(host.innerSwapChain_->QueryInterface(IID_PPV_ARGS(&chain)))) { return false; }
        host.sourceUIBufferIndex_ = chain->GetCurrentBackBufferIndex();
        InternalOperation internal(host.sourceUIInternal_);
        return host.PrepareNativeUITarget(host.sourceUIBufferIndex_);
    }

    bool NativeUIAvailable() { return host.nativeUI_.Available(); }
    bool ComposeNativeUI()
    {
        ScopedD3D11PerformanceStage timer{host.context_.Get(), PerformanceTuning::D3D11Stage::kNativeUIComposition};
        const bool composed = host.nativeUI_.Dedicated() ? host.CaptureAndComposeDedicatedNativeUI() : host.ExtractNativeUIColorAndAlpha();
        return composed;
    }
    void DisableRuntime() { host.SetRuntimeEnabled(false); }
    void CompositionFailed()
    {
        host.nativeUI_.Invalidate();
        logger::error("[NativeUIRoute] composition failed; reverting to HUD-less-only tagging");
    }
    void Trace(const char* stage) { host.LogNativeUIState(stage, MainOrLoading()); }
};

void NvidiaHost::OnBackgroundReady(TheosRenderPipeline::BackgroundBoundary boundary, bool mainOrLoading)
{
    if (!StartupConfigured() || !proxyActive_ || !UpscalerReady() || !context_ || !RenderPipeline::GetSingleton()->mNativeUI) { return; }
    if (boundary == TheosRenderPipeline::BackgroundBoundary::World) {
        startupWorldFrame_ = presentCount_;
        sourceRenderThread_ = GetCurrentThreadId();
    }
    SourceFrameOperations operations{*this};
    auto* ui = RE::UI::GetSingleton();
    const bool world = boundary == TheosRenderPipeline::BackgroundBoundary::World;
    const bool mainMenu = ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
    const bool loadingMenu = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
    TheosRenderPipeline::LoadingArtwork::Boundary(presentCount_, world, mainMenu, loadingMenu);
    loadingScreenRoute_.Boundary(presentCount_, world, loadingMenu, mainMenu);
    sourceFrameCoordinator_.OnBackgroundReady(context_.Get(), operations, boundary, mainOrLoading);
}

bool NvidiaHost::PrepareSourceFrameForPresent(IDXGISwapChain* swapChain)
{
    if (FAILED(FailureResult()) || !StartupConfigured() || swapChain != outerSwapChain_ || !context_) { return false; }
    if (startupOverlay_.Active()) {
        logger::warn("[StartupOverlay] producer crossed into Present; restoring its target state");
        EndStartupOverlay();
        if (startupOverlay_.Active()) { return false; }
    }
    SourceFrameOperations operations{*this};
    return sourceFrameCoordinator_.PrepareForPresent(context_.Get(), operations);
}

bool NvidiaHost::FinishSourceFrameForPresent()
{
    const bool ready = NativePresentReady();
    SourceFrameOperations operations{*this};
    const bool finished = sourceFrameCoordinator_.FinishForPresent(operations);
    if (finished && ready && startupOverlay_.Drawn(presentCount_)) {
        ScopedD3D11PerformanceStage timer{context_.Get(), PerformanceTuning::D3D11Stage::kStartupOverlayComposition};
        InternalOperation internal(sourceUIInternal_);
        // The scene and ordinary native UI are complete. This foreground was
        // never included in DLSS input and must survive a same-frame Mist entry.
        if (!nativeUI_.ComposeOverlay(context_.Get(), presentation_.Texture(), startupOverlay_.SRV())) {
            logger::error("[StartupOverlay] native foreground composition failed");
            SetRuntimeEnabled(false);
        } else if (TakeNativeUITraceSlot()) {
            logger::info("[StartupOverlay] frame={} composed native foreground {}x{}", presentCount_, outputWidth_, outputHeight_);
        }
    }
    startupOverlay_.Consume();
    return finished;
}

bool NvidiaHost::QueryStartupOverlay() const
{
    return StartupConfigured() && proxyActive_ && UpscalerReady() && context_ &&
        RenderPipeline::GetSingleton()->mNativeUI && RenderPipeline::GetSingleton()->mWheelerLateOverlayBridge &&
        nativeUI_.Available() && nativeUI_.Dedicated() &&
        outputWidth_ && outputHeight_ &&
        sourceRenderThread_ == GetCurrentThreadId() && !NativeUIInternalBind() &&
        !startupOverlay_.Active() && TheosRenderPipeline::StartupOverlayEligible(
            nativeUIPass_.Frame(), MainOrLoading(), startupWorldFrame_ == presentCount_, nativeUIPass_.Active());
}

bool NvidiaHost::BeginStartupOverlay()
{
    if (!QueryStartupOverlay() || !gameTargets_.UpscaleOutput()) { return false; }
    D3D11_TEXTURE2D_DESC output{};
    gameTargets_.UpscaleOutput()->GetDesc(&output);
    if (!startupOverlay_.Ensure(device_.Get(), output) ||
        !startupOverlay_.Begin(context_.Get(), presentCount_, true)) { return false; }
    if (TakeNativeUITraceSlot()) {
        logger::info("[StartupOverlay] frame={} begin native foreground {}x{}; scene evaluation deferred", presentCount_, outputWidth_, outputHeight_);
    }
    return true;
}

void NvidiaHost::EndStartupOverlay()
{
    if (!startupOverlay_.End()) { logger::error("[StartupOverlay] unmatched or cross-thread end rejected"); }
}
