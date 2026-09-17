#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "GameFacingTargets.h"
#include "NativeUIAttachments.h"
#include "NativeUIComposition.h"
#include "NativeUIContexts.h"
#include "NativeUIPass.h"
#include "NvidiaUpscalerConfiguration.h"
#include "PresentationTargets.h"
#include "SourceFrameCoordinator.h"
#include "SourceHostBoundary.h"
#include "StartupOverlayPass.h"
#include "InventoryPreviewDraw.h"
#include "LoadingScreenState.h"
#include "LoadingScreenUpscaler.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <sl_dlss_g.h>
#include <string>
#include <vector>

class NvidiaHost
{
  public:
    static NvidiaHost* GetSingleton()
    {
        static NvidiaHost singleton;
        return &singleton;
    }

    HRESULT CreateSwapChain(IDXGIFactory* a_factory, ID3D11Device* a_device, DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_swapChain);
    bool CompleteStartupAfterDeviceCreation();
    bool EvaluateFrame(IDXGISwapChain* a_swapChain, bool a_nativeUIHandoff = false);
    bool FinishNativeUIPassForPresent();
    void OnBackgroundReady(TheosRenderPipeline::BackgroundBoundary boundary, bool mainOrLoading);
    bool PrepareSourceFrameForPresent(IDXGISwapChain* swapChain);
    bool QueryStartupOverlay() const;
    bool BeginStartupOverlay();
    void EndStartupOverlay();
    TheosRenderPipeline::StartupOverlayPass& StartupOverlay() { return startupOverlay_; }
    TheosRenderPipeline::InventoryPreviewDraw& PreviewDraw() { return previewDraw_; }
    bool NativePresentReady() const { return StartupConfigured() && nativeUIPass_.Frame().PresentPrepared() && nativeUIPass_.HasEarlyEvaluation(); }
    bool NativeUIDrawnThisFrame() const { return nativeUIPass_.Frame().UIDrawn(); }
    const TheosRenderPipeline::NativeUIFrame& NativeFrame() const { return nativeUIPass_.Frame(); }
    const TheosRenderPipeline::NativeUIAttachments& UIAttachments() const { return nativeUIAttachments_; }
    bool SourceContext(ID3D11DeviceContext* context) const { return StartupConfigured() && nativeUIContexts_.Contains(context_.Get(), context); }
    void RegisterSourceGameContext(ID3D11DeviceContext* context);
    void LogSourceContextHook(unsigned hook, ID3D11DeviceContext* context) const;
    void LogNativeUIState(const char* stage, bool mainOrLoading);
    bool TakeNativeUITraceSlot();
    HRESULT GetGameFacingBuffer(IDXGISwapChain* a_swapChain, UINT a_buffer, REFIID a_iid, void** a_surface);
    void AdjustLegacyDescForCaller(DXGI_SWAP_CHAIN_DESC* a_desc, const void* a_returnAddress) const;
    HRESULT BeforeResizeBuffers(IDXGISwapChain* a_swapChain);
    HRESULT AfterResizeBuffers(IDXGISwapChain* a_swapChain, HRESULT a_result);
    HRESULT FailureResult() const { return lifecycleFailure_.Result(); }
    HRESULT FailLifecycle(HRESULT result, const char* operation);
    void OnPresentCompleted(HRESULT a_result);
    void OnGameFacingSwapChainDestroyed(IDXGISwapChain* a_swapChain);

    bool ProxyActive() const { return proxyActive_; }
    bool FrameGenerationEnabled() const { return frameGenerationEnabled_; }
    bool UpscalerReady() const { return upscalerReady_ && SUCCEEDED(FailureResult()); }
    bool SplitSourceDLSSActive() const { return splitSourceDLSSActive_; }
    bool StartupConfigured() const { return sourceUpscalerSettings_.Initialized(); }
    const TheosRenderPipeline::Upscaler::Configuration& SourceUpscalerSettings() const { return sourceUpscalerSettings_; }
    void RequestSourceUpscalerSettings(TheosRenderPipeline::Upscaler::Creation request);
    void SourceUpscalerSettingsSaved() { sourceUpscalerSettings_.Saved(); }
    void AdoptEffectiveSourceUpscalerSettings() const;
    UINT OutputWidth() const { return outputWidth_; }
    UINT OutputHeight() const { return outputHeight_; }
    UINT RenderWidth() const { return renderWidth_; }
    UINT RenderHeight() const { return renderHeight_; }
    HWND GameWindow() const { return outputWindow_; }
    ID3D11Texture2D* GameFacingTexture() const { return gameTargets_.GameFacing(); }
    ID3D11Texture2D* NativePresentationTexture() const { return presentation_.Texture(); }
    ID3D11RenderTargetView* NativePresentationRTV() const { return presentation_.RTV(); }
    ID3D11Texture2D* NativeUIRenderTexture() const { return nativeUI_.Dedicated() ? nativeUI_.RenderTexture() : presentation_.Texture(); }
    ID3D11RenderTargetView* NativeUIRenderRTV() const { return nativeUI_.Dedicated() ? nativeUI_.RenderRTV() : presentation_.RTV(); }
    ID3D11DepthStencilView* NativeUIDepthDSV() const { return presentation_.DepthDSV(); }
    bool DedicatedUITextureMode() const { return nativeUI_.Dedicated(); }
    bool NativeUIPassActive() const { return nativeUIPass_.Active(); }
    bool NativeUIInternalBind() const { return nativeUIPass_.InternalBind() || sourceUIInternal_ || startupOverlay_.Internal() || previewDraw_.Internal(); }
    ID3D11DepthStencilView* NativeUIBackgroundDepth() const { return nativeUIPass_.BackgroundDepth(); }
    bool NativeUITargetBound() const { return nativeUIPass_.TargetBound(); }
    void SetNativeUITargetBound(bool a_bound) { nativeUIPass_.SetTargetBound(a_bound); }
    float OptimalMipmapBias() const;
    std::uint64_t EvaluationCount() const { return evaluationCount_; }
    std::int32_t WarmupPresentsRemaining() const { return warmupPresentsRemaining_; }
    std::uint64_t PresentCount() const { return presentCount_; }
    std::uint64_t FailedPresentCount() const { return failedPresentCount_; }
    HRESULT LastPresentResult() const { return lastPresentResult_; }
    std::uint64_t RuntimeStateObservationCount() const { return runtimeStateObservationCount_; }
    std::uint32_t RuntimeDLSSGStatus() const { return runtimeDLSSGStatus_; }
    std::uint32_t RuntimeFramesActuallyPresented() const { return runtimeFramesActuallyPresented_; }
    std::uint32_t RuntimeMinWidthOrHeight() const { return runtimeMinWidthOrHeight_; }
    std::uint32_t RuntimeMaxGeneratedFrames() const { return runtimeMaxGeneratedFrames_; }
    const std::string& Status() const { return status_; }

  private:
    NvidiaHost() = default;
    struct LifecycleOperations;
    void ResetSessionAfterRetirement();

    bool CreateGameFacingResources(IDXGISwapChain* a_swapChain);
    bool InitializeSourceUpscaler(const D3D11_TEXTURE2D_DESC& a_outputDesc);
    bool CreateNativeUIExtractionResources(const D3D11_TEXTURE2D_DESC& a_outputDesc);
    bool ExtractNativeUIColorAndAlpha();
    bool CaptureAndComposeDedicatedNativeUI();
    bool PresentationBackendReadyForEvaluation();
    void UpdateRuntimeDLSSGState(std::uint32_t a_status, std::uint32_t a_framesActuallyPresented, std::uint32_t a_minWidthOrHeight,
                                 std::uint32_t a_maxGeneratedFrames);
    bool PrepareNativeUITarget(UINT a_bufferIndex);
    bool PrepareSourceNativeUITargets();
    struct SourceFrameOperations;
    struct SourceNvidiaEvaluationOperations;
    bool EvaluateSourceNvidiaFrame(bool nativeUIHandoff, bool resetHistory);
    bool FinishSourceFrameForPresent();
    void EndNativeUIPass();
    void ReleaseSourceUpscaler();
    void ArmFrameGenerationWarmup();
    void SetRuntimeEnabled(bool a_enabled);
    void ApplySourceUpscalerSettingsAfterPresent();
    TheosRenderPipeline::Upscaler::Configuration sourceUpscalerSettings_;
    TheosRenderPipeline::SourceHostFailure lifecycleFailure_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    TheosRenderPipeline::NativeUIContexts nativeUIContexts_;
    TheosRenderPipeline::GameFacingTargets gameTargets_;
    TheosRenderPipeline::NativeUIComposition nativeUI_;
    TheosRenderPipeline::PresentationTargets presentation_;
    TheosRenderPipeline::NativeUIPass nativeUIPass_;
    TheosRenderPipeline::SourceFrameCoordinator sourceFrameCoordinator_{nativeUIPass_};
    TheosRenderPipeline::StartupOverlayPass startupOverlay_;
    TheosRenderPipeline::InventoryPreviewDraw previewDraw_;
    TheosRenderPipeline::LoadingScreenUpscaler loadingScreenUpscaler_;
    TheosRenderPipeline::LoadingScreenRoute loadingScreenRoute_;
    HRESULT loadingScreenResult_{S_OK};
    bool loadingScreenLogged_{};
    std::uint64_t startupWorldFrame_{~std::uint64_t{}};
    DWORD sourceRenderThread_{};
    TheosRenderPipeline::NativeUIAttachments nativeUIAttachments_;
    bool sourceUIInternal_{};
    std::uint32_t sourceUIBufferIndex_{};
    IDXGISwapChain* innerSwapChain_{nullptr};
    IDXGISwapChain* outerSwapChain_{nullptr};
    HWND outputWindow_{nullptr};
    UINT outputWidth_{0};
    UINT outputHeight_{0};
    UINT renderWidth_{0};
    UINT renderHeight_{0};
    bool proxyActive_{false};
    bool sourceUpscalerInitializationPending_{false};
    // Retained feature ownership can outlive a terminal failure. Runtime callers
    // must use UpscalerReady(), which also checks the latched lifecycle error.
    bool upscalerReady_{false};
    bool splitSourceDLSSActive_{false};
    bool splitSourceRuntimeFailureLogged_{false};
    bool frameGenerationStateKnown_{false};
    bool frameGenerationEnabled_{false};
    bool resetNextEvaluation_{true};
    bool evaluationFailureLogged_{false};
    bool nativeUIExtractionFailureLogged_{false};
    std::int32_t warmupPresentsRemaining_{0};
    std::uint64_t upscaleEvaluationCount_{0};
    std::uint64_t evaluationCount_{0};
    std::uint64_t presentCount_{0};
    std::uint64_t failedPresentCount_{0};
    HRESULT lastPresentResult_{S_OK};
    std::uint64_t runtimeStateObservationCount_{0};
    std::uint32_t runtimeDLSSGStatus_{0};
    std::uint32_t runtimeFramesActuallyPresented_{0};
    std::uint32_t runtimeMinWidthOrHeight_{0};
    std::uint32_t runtimeMaxGeneratedFrames_{0};
    std::string status_{"not requested"};
    std::string sourceSuccessStatus_;
    bool sourceSuccessValid_{};
    std::int32_t sourceSuccessWarmup_{};
};
