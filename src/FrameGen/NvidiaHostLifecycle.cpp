#include "DLSSBackend.h"
#include "RenderPipeline.h"
#include "NativeInput.h"
#include "NvidiaHost.h"
#include "LoadingArtwork.h"
#include "SourceDLSSGBackend.h"
#include "SourceDLSSGCamera.h"
#include "SourceFrameGeneration.h"
#include <PCH.h>

#include "SourceHostLifecycle.h"

struct NvidiaHost::LifecycleOperations
{
    NvidiaHost& host;
    IDXGISwapChain* swapChain{};
    bool RebuildGameFacing() { return host.CreateGameFacingResources(swapChain); }
    bool RebuildUpscaler() { return host.CompleteStartupAfterDeviceCreation(); }
    void RequestHistoryReset() { host.resetNextEvaluation_ = true; }
    HRESULT Fail(HRESULT result, const char* operation) { return host.FailLifecycle(result, operation); }
    void DisableGeneration() { host.SetRuntimeEnabled(false); }
    bool Retire() { return TheosRenderPipeline::SourceDLSSG::Backend::Get().Quiesce(); }
    void EndUI() { host.EndNativeUIPass(); }
    void ClearAndFlush()
    {
        host.context_->ClearState();
        host.context_->Flush();
    }
    void ReleaseGameFacing() { host.gameTargets_.ResetGameFacingAfterRetirement(); }
    void ReleaseUpscaler() { host.ReleaseSourceUpscaler(); }
    void ReleasePresentation() { host.presentation_.ResetAfterRetirement(); }
    void UnpublishInput() { TheosRenderPipeline::NativeInput::Publish(0, 0); }
    void DetachFailedHost()
    {
        host.proxyActive_ = false;
        host.outerSwapChain_ = nullptr;
    }
    void BeginDestruction()
    {
        host.proxyActive_ = false;
        host.sourceUpscalerInitializationPending_ = false;
    }
    void DetachSwapchains()
    {
        host.outerSwapChain_ = nullptr;
        host.innerSwapChain_ = nullptr;
    }
    void ResetSession() { host.ResetSessionAfterRetirement(); }
};

HRESULT NvidiaHost::FailLifecycle(HRESULT result, const char* operation)
{
    if (FAILED(FailureResult())) { return FailureResult(); }
    const auto failure = lifecycleFailure_.Fail(result);
    const auto detail = status_;
    sourceUpscalerInitializationPending_ = false;
    SetRuntimeEnabled(false);
    TheosRenderPipeline::NativeInput::Publish(0, 0);
    status_ = std::format("{} failed (0x{:08X}); rendering stopped, restart required. {}",
        operation, static_cast<std::uint32_t>(failure), detail);
    logger::critical("[NvidiaHost] {}", status_);
    // Keep ownership until the normal teardown proves retirement. In particular,
    // do not clear the feature-exists flags used by ReleaseSourceUpscaler here.
    return failure;
}

HRESULT NvidiaHost::BeforeResizeBuffers(IDXGISwapChain* a_swapChain)
{
    if (FAILED(FailureResult())) { return FailureResult(); }
    if (!proxyActive_ || a_swapChain != innerSwapChain_)
    {
        return S_OK;
    }
    LifecycleOperations operations{*this};
    return TheosRenderPipeline::SourceHostLifecycle::BeforeResize(operations) ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING;
}

HRESULT NvidiaHost::AfterResizeBuffers(IDXGISwapChain* a_swapChain, HRESULT a_result)
{
    if (FAILED(FailureResult())) { return FailureResult(); }
    if (!proxyActive_ || a_swapChain != innerSwapChain_)
    {
        return a_result;
    }
    LifecycleOperations operations{*this, a_swapChain};
    return TheosRenderPipeline::SourceHostLifecycle::AfterResize(operations, a_result);
}

void NvidiaHost::OnGameFacingSwapChainDestroyed(IDXGISwapChain* a_swapChain)
{
    if (a_swapChain != outerSwapChain_)
    {
        return;
    }
    LifecycleOperations operations{*this};
    TheosRenderPipeline::SourceHostLifecycle::Destroy(operations);
}

void NvidiaHost::ResetSessionAfterRetirement()
{
    outputWindow_ = nullptr;
    outputWidth_ = 0;
    outputHeight_ = 0;
    renderWidth_ = 0;
    renderHeight_ = 0;
    runtimeStateObservationCount_ = 0;
    runtimeDLSSGStatus_ = 0;
    runtimeFramesActuallyPresented_ = 0;
    runtimeMinWidthOrHeight_ = 0;
    runtimeMaxGeneratedFrames_ = 0;
    presentCount_ = 0;
    failedPresentCount_ = 0;
    lastPresentResult_ = S_OK;
    warmupPresentsRemaining_ = 0;
    nativeUIContexts_.ResetAfterRetirement();
    context_.Reset();
    device_.Reset();
}

void NvidiaHost::ReleaseSourceUpscaler()
{
    EndNativeUIPass();
    startupOverlay_.ResetAfterRetirement();
    previewDraw_.ResetAfterRetirement();
    TheosRenderPipeline::LoadingArtwork::ResetAfterRetirement();
    loadingScreenUpscaler_.ResetAfterRetirement();
    loadingScreenRoute_.ResetAfterRetirement();
    loadingScreenResult_ = S_OK;
    loadingScreenLogged_ = false;
    startupWorldFrame_ = ~std::uint64_t{};
    sourceRenderThread_ = 0;
    nativeUIPass_.ResetEvaluation();
    nativeUIAttachments_.ResetAfterRetirement();
    nativeUI_.ResetAfterRetirement();
    gameTargets_.ResetUpscalerAfterRetirement();
    if (upscalerReady_ && splitSourceDLSSActive_)
    {
        DLSSBackend::GetSingleton()->ReleaseFeature();
    }
    upscalerReady_ = false;
    splitSourceDLSSActive_ = false;
}

void NvidiaHost::OnPresentCompleted(HRESULT a_result)
{
    if (!proxyActive_ || FAILED(FailureResult()))
    {
        return;
    }

    ++presentCount_;
    const auto previousResult = lastPresentResult_;
    lastPresentResult_ = a_result;
    if (FAILED(a_result))
    {
        ++failedPresentCount_;
        if (failedPresentCount_ <= 3 || a_result != previousResult)
        {
            logger::error("[NvidiaHost] outer Present failed result=0x{:08X} "
                          "failures={} presents={}",
                          static_cast<std::uint32_t>(a_result), failedPresentCount_, presentCount_);
        }
    }
    else if (FAILED(previousResult))
    {
        logger::info("[NvidiaHost] outer Present recovered result=0x{:08X} "
                     "failures={} presents={}",
                     static_cast<std::uint32_t>(a_result), failedPresentCount_, presentCount_);
    }

    // Consume the session snapshot after Present; querying Streamline again
    // here would consume its output-count delta a second time.
    const auto& state = TheosRenderPipeline::SourceDLSSG::Backend::Get().Snapshot().state;
    UpdateRuntimeDLSSGState(static_cast<std::uint32_t>(state.status), state.numFramesActuallyPresented, state.minWidthOrHeight,
                            state.numFramesToGenerateMax);

    if (StartupConfigured() && SUCCEEDED(a_result))
    {
        ApplySourceUpscalerSettingsAfterPresent();
        if (FAILED(FailureResult())) { return; }
    }
    if (warmupPresentsRemaining_ <= 0)
    {
        return;
    }

    // Advance host warm-up after every real outer Present, including failed
    // DXGI calls. Do not enable generation at this boundary:
    // the next valid evaluation must still prove complete color, depth, motion,
    // and presentation inputs before TheosRenderPipeline crosses the runtime boundary.
    --warmupPresentsRemaining_;
    if (warmupPresentsRemaining_ == 0)
    {
        logger::info("[NvidiaHost] 600-Present host warm-up complete "
                     "result=0x{:08X}; waiting for next valid frame inputs",
                     static_cast<std::uint32_t>(a_result));
        status_ = "NVIDIA host warm-up complete; waiting for valid frame inputs";
    }
    else if (warmupPresentsRemaining_ == 599 || warmupPresentsRemaining_ % 120 == 0)
    {
        logger::info("[NvidiaHost] host warm-up presents remaining={} lastResult=0x{:08X}", warmupPresentsRemaining_,
                     static_cast<std::uint32_t>(a_result));
    }
}

void NvidiaHost::ArmFrameGenerationWarmup()
{
    static constexpr std::int32_t kHostWarmupPresents = 600;
    warmupPresentsRemaining_ = kHostWarmupPresents;
    SetRuntimeEnabled(false);
    status_ = std::format("NVIDIA source ready; host warm-up {} Presents remaining", warmupPresentsRemaining_);
    logger::info("[NvidiaHost] armed host warm-up presents={}", warmupPresentsRemaining_);
}

void NvidiaHost::SetRuntimeEnabled(bool a_enabled)
{
    if (frameGenerationStateKnown_ && frameGenerationEnabled_ == a_enabled)
    {
        return;
    }
    TheosRenderPipeline::SourceDLSSG::Backend::Get().SetEnabled(a_enabled);
    frameGenerationStateKnown_ = true;
    frameGenerationEnabled_ = a_enabled;
    if (!a_enabled)
    {
        resetNextEvaluation_ = true;
    }
    logger::info("[NvidiaHost] runtime generation {}", a_enabled ? "enabled" : "disabled");
}

void NvidiaHost::UpdateRuntimeDLSSGState(std::uint32_t a_status, std::uint32_t a_framesActuallyPresented, std::uint32_t a_minWidthOrHeight,
                                         std::uint32_t a_maxGeneratedFrames)
{
    const bool changed = runtimeDLSSGStatus_ != a_status || runtimeFramesActuallyPresented_ != a_framesActuallyPresented ||
                         runtimeMinWidthOrHeight_ != a_minWidthOrHeight || runtimeMaxGeneratedFrames_ != a_maxGeneratedFrames;
    runtimeDLSSGStatus_ = a_status;
    runtimeFramesActuallyPresented_ = a_framesActuallyPresented;
    runtimeMinWidthOrHeight_ = a_minWidthOrHeight;
    runtimeMaxGeneratedFrames_ = a_maxGeneratedFrames;
    ++runtimeStateObservationCount_;

    const auto observationCount = RuntimeStateObservationCount();
    if (observationCount <= 3 || changed || observationCount % 600 == 0)
    {
        logger::info("[NvidiaHost] runtime DLSS-G state source=source-session observation={} "
                     "status={} actuallyPresented={} maxGenerated={} minDimension={}",
                     observationCount, a_status, a_framesActuallyPresented, a_maxGeneratedFrames, a_minWidthOrHeight);
    }
}
