#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl_consts.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xess_fg/xefg_swapchain_debug.h>
#include <xell/xell_d3d12.h>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "XeFGOptions.h"
#include "XeFGTensorRuntime.h"
#include "PresenterFactory.h"

namespace TheosRenderPipeline
{
    struct XeFGSnapshot
    {
        std::uint32_t frameId{}, framesPresented{};
        std::uint64_t presents{}, totalOutputs{}, epoch{1}, generatedPresents{}, warnings{}, uiTexturePresents{};
        int interpolationResult{};
        bool enabled{}, prepared{}, invertedDepth{}, uiTexture{};
        bool onlyGenerated{}, tagGenerated{};
        std::uint32_t frameLimitUs{};
        float frameTimeMs{}; // Last frameRenderTime sent; 0 lets Intel estimate.
        float sleepMs{}; // CPU time in the last XeLL sleep.
        float sceneChangeThreshold{-1}; // Last value Intel accepted; -1 is Intel's default.
        std::uint32_t maxGeneratedFrames{1}, generatedFrames{1};
        bool experimentalMFG{}, unlockReady{};
        bool tensorRequested{}, tensorModel{}, tensorActive{};
        std::uint64_t tensorLaunches{}, tensorFallbacks{};
        XeFGPacing::Stats pacing;
    };

    // One Intel-owned presenter and XeLL owner. DLLs remain process-resident.
    // ONLY_NOW tags copy inputs on the caller's command list; the host retires
    // that submission before reusing shared producer textures. Destroy retires
    // the SDK's private work; a host fence alone does not prove that retirement.
    class XeFGPresenter final
    {
    public:
        HRESULT Probe(ID3D12Device* device, const std::filesystem::path& directory, bool tensorRequested = false);
        void SetLogger(void (*callback)(const char*)) { log_ = callback; }
        HRESULT Create(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGIFactory* factory,
            const DXGI_SWAP_CHAIN_DESC& desc, bool inverted, IDXGISwapChain** swapchain,
            std::uint32_t lastApplicationFrame = 0, XeFGOptions options = {}, ID3D12PipelineLibrary* pipelines = nullptr,
            IDXGISwapChain* existingNative = nullptr);
        HRESULT BeginFrame();
        IDXGISwapChain* NativeSwapChain() const { return factoryCapture_ ? factoryCapture_->Native() : nullptr; }
        bool HasContext() const { return fg_ != nullptr; }
        bool HasLatencyContext() const { return ll_ != nullptr; }
        // frameTime sends the measured interval between application frames as
        // frameRenderTime; otherwise Intel uses its own estimate.
        HRESULT Prepare(const sl::Constants& camera, bool frameTime = false);
        // Intel debug features; applied when changed and again after recreation.
        HRESULT SetDebugView(bool onlyGenerated, bool tagGenerated);
        // Scene-change sensitivity, 0-1; applied when changed and after recreation.
        // A runtime without the call, or one refusing the value, keeps the value in
        // effect (Intel's default unless set) and generation continues.
        void SetSceneChangeThreshold(float threshold);
        // ui is optional: a premultiplied UI-only layer matching the HUD-less
        // format and size. AUTO composition blends it when tagged and otherwise
        // extracts the UI from the back buffer.
        HRESULT BeforePresent(ID3D12GraphicsCommandList* list, ID3D12Resource* motion,
            ID3D12Resource* depth, ID3D12Resource* hudless, ID3D12Resource* ui, bool enabled, bool uiComposition,
            int outputFPSLimit, std::uint32_t generatedFrames = 1);
        HRESULT AfterPresent(HRESULT result);
        HRESULT FinalizePresent(); // After the host submits the input-copy command list.
        HRESULT Disable();
        // Caller first releases all swapchain/buffer references after its queue
        // retires. A failure leaves context handles retained and stops replacement.
        HRESULT Destroy();
        bool Initialized() const { return initialized_; }
        // The SDK copy the next Create uses, which Probe chose: Intel's own folder
        // or the tensor bundle. Both stay loaded once used and share one XeLL.
        HMODULE RuntimeModule() const { const auto wanted = LoadedRuntime(wanted_); return wanted ? wanted : fgModule_; }
        const char* PipelineCacheName() const { return wantTensor_ ? "xefg-tensor-v1" : "xefg"; }
        bool TensorWanted() const { return wantTensor_; }
        bool TensorInUse() const { return usingTensor_; }
        // The tensor request the last Probe saw; a different request needs a new presenter.
        bool ProbedTensorRequest() const { return probedTensorRequest_; }
        void ResetHistory() { needsReset_ = true; snapshot_.prepared = false; ResetPacing(); }
        const XeFGSnapshot& Snapshot() const { return snapshot_; }
        const std::string& Status() const { return status_; }
        const std::string& UnlockStatus() const { return unlockStatus_; }
        const std::string& TensorStatus() const { return tensor_.Status(); }
        XeFGTensor::Stats TensorStats() const { return tensor_.Snapshot(); }
        XeFGPacing::Stats PacingStats() const { return tensor_.PacingStats(); }
        const std::string& PacingStatus() const { return tensor_.PacingStatus(); }
        bool FlipMeteringRequested() const { return tensor_.FlipMeteringRequested(); }
        void SetFlipMetering(bool enabled) { tensor_.SetFlipMetering(enabled); }
        static bool SupportsFormat(DXGI_FORMAT format);
    private:
        HRESULT FG(xefg_swapchain_result_t result, const char* operation);
        HRESULT LL(xell_result_t result, const char* operation);
        // Loads a folder's libxess_fg beside any already loaded; never switches.
        HRESULT Load(const std::filesystem::path& directory);
        // Binds the API to a loaded copy. Only while no presenter context exists.
        HRESULT Activate(const std::filesystem::path& directory);
        HMODULE LoadedRuntime(const std::filesystem::path& directory) const;
        void ResetPacing() { if (pacingRegistered_) tensor_.ResetPacing(fg_); }
        void Trace(const char* stage) { status_ = stage; if (log_) { log_(stage); } }
        void (*log_)(const char*){};
        bool apiReady_{};
        Microsoft::WRL::ComPtr<PresenterFactory> factoryCapture_;
        HMODULE fgModule_{}, llModule_{};
        std::filesystem::path directory_;
        std::vector<std::pair<std::filesystem::path, HMODULE>> runtimes_; // Loaded libxess_fg copies.
        std::filesystem::path wanted_;
        bool wantTensor_{}, usingTensor_{}, probedTensorRequest_{}, pacingRegistered_{};
        ID3D12Device* admittedDevice_{}; // Identity only; the backend owns the device.
#define TRP_FG_FUNCTION(name) decltype(&::name) name##_{};
#define TRP_LL_FUNCTION(name) decltype(&::name) name##_{};
        TRP_FG_FUNCTION(xefgSwapChainGetVersion)
        TRP_FG_FUNCTION(xefgSwapChainSetLoggingCallback)
        TRP_FG_FUNCTION(xefgSwapChainD3D12CreateContext)
        TRP_FG_FUNCTION(xefgSwapChainSetLatencyReduction)
        TRP_FG_FUNCTION(xefgSwapChainGetProperties)
        TRP_FG_FUNCTION(xefgSwapChainD3D12InitFromSwapChainDesc)
        TRP_FG_FUNCTION(xefgSwapChainD3D12GetSwapChainPtr)
        TRP_FG_FUNCTION(xefgSwapChainD3D12TagFrameResource)
        TRP_FG_FUNCTION(xefgSwapChainTagFrameConstants)
        TRP_FG_FUNCTION(xefgSwapChainSetPresentId)
        TRP_FG_FUNCTION(xefgSwapChainSetEnabled)
        TRP_FG_FUNCTION(xefgSwapChainSetNumInterpolatedFrames)
        TRP_FG_FUNCTION(xefgSwapChainEnableDebugFeature)
        TRP_FG_FUNCTION(xefgSwapChainGetLastPresentStatus)
        TRP_FG_FUNCTION(xefgSwapChainSetUiCompositionState)
        TRP_FG_FUNCTION(xefgSwapChainDestroy)
        TRP_FG_FUNCTION(xefgSwapChainSetSceneChangeThreshold) // Optional.
        TRP_LL_FUNCTION(xellGetVersion)
        TRP_LL_FUNCTION(xellD3D12CreateContext)
        TRP_LL_FUNCTION(xellSetSleepMode)
        TRP_LL_FUNCTION(xellSleep)
        TRP_LL_FUNCTION(xellAddMarkerData)
        TRP_LL_FUNCTION(xellDestroyContext)
#undef TRP_FG_FUNCTION
#undef TRP_LL_FUNCTION
        xefg_swapchain_handle_t fg_{};
        xell_context_handle_t ll_{};
        xefg_swapchain_frame_constant_data_t constants_{};
        XeFGSnapshot snapshot_;
        std::string status_{"not initialized"};
        std::string unlockStatus_{"Experimental MFG off; official x2"};
        int lastWarning_{};
        std::string lastWarningOperation_;
        bool initialized_{}, frameBegun_{}, presentPending_{}, needsReset_{true};
        std::chrono::steady_clock::time_point lastFrameBegin_{};
        float frameIntervalMs_{};
        bool debugOnlyGenerated_{}, debugTagGenerated_{};
        std::optional<float> sceneChangeThreshold_; // Last value sent to this context.
        XeFGTensorRuntime tensor_;
        bool tensorChoiceMade_{};
        void* retiredPacingContext_{}; // Identity only, after SDK Destroy succeeds.
        std::atomic<HRESULT> nativeCreationResult_{S_OK};
    };
}
