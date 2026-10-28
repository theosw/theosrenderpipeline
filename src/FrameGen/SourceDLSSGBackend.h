#pragma once
#include "SteadyPacing.h"
#include <chrono>
#include "SourceRetirementPolicy.h"
#include "PresentationVSync.h"

#include "SourceDLSSGInterop.h"
#include "SourceDLSSGDeviceLoss.h"
#include "D3D11FrameCopy.h"
#include "SourceDLSSGSession.h"
#include "DlssgPreset.h"
#include "SourceDLSSGNeuralState.h"
#if !defined(TRP_NO_NEURAL_RENDERING)
#include "SourceDLSSGNeuralRendering.h"
#include "SourceDLSSGNeuralAvailability.h"
#endif
#include "SourceDLSSGHDR.h"
#include "SourceDLSSGHDROutput.h"
#include "FinalFrameCapture.h"
#include "../EngineScreenshot.h"
#include "SourceDLSSGMFG.h"
#include "SourceDLSSGPresentationFeedback.h"
#include "SourceDLSSGRuntimeDiagnostics.h"
#include "FrameGenerationProvider.h"
#include "XeFGPresenter.h"
#include "XeSSUpscaler.h"
#include "SlowPresentGate.h"
#include "PipelineCache.h"
#include "XeFGSettleGate.h"
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
#include "FrameGen/SourceOutputCapture.h"
#endif
#include <dxgi1_5.h>
#include <vector>
#include <filesystem>
#include <array>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>

namespace TheosRenderPipeline::SourceDLSSG
{
	class SwapChain;
	// Process-resident Streamline owner. Rejects competing runtime owners
	// and never unloads plugin code while DXGI proxy objects could still call it.
	class Backend final
	{
	public:
		static Backend& Get();
		void RequestProvider(FrameGenerationProvider provider) { if (ValidProvider(static_cast<int>(provider))) { requestedProvider_.store(provider); } }
		FrameGenerationProvider Provider() const { return provider_; }
		FrameGenerationProvider RequestedProvider() const { return requestedProvider_.load(); }
		const XeFGSnapshot& XeFGState() const { return xefg_.Snapshot(); }
		const std::string& XeFGUnlockStatus() const { return xefg_.UnlockStatus(); }
		const std::string& XeFGTensorStatus() const { return xefg_.TensorStatus(); }
		// Live: a running XeFG presenter is recreated on the other Intel SDK copy.
		void ConfigureXeFGTensor(bool requested) { xefgTensorRequested_.store(requested); }
		bool XeFGTensorRequested() const { return xefgTensorRequested_.load(); }
		// Whether the running presenter uses the tensor model's SDK copy.
		bool XeFGTensorInUse() const { return xefg_.TensorInUse(); }
		const std::string& XeFGPacingStatus() const { return xefg_.PacingStatus(); }
		void ConfigureXeFGFlipMetering(bool enabled) { xefgFlipMetering_.store(enabled, std::memory_order_relaxed); }
		bool XeFGFlipMetering() const { return xefgFlipMetering_.load(std::memory_order_relaxed); }
		void ConfigureVSync(VSyncMode mode) { vsyncMode_.store(SanitizeVSync(mode)); }
		VSyncMode VSyncConfigurationMode() const { return vsyncMode_.load(); }
		VSyncConfiguration PresentationConfiguration() const;
		struct VSyncObservation { bool observed{}; VSyncSubmission submission{}; HRESULT result{S_FALSE}; FrameGenerationProvider provider{}; };
		void ObserveVSync(VSyncSubmission submission, HRESULT result);
		VSyncObservation VSyncState() const { std::scoped_lock lock(vsyncMutex_); return vsyncState_; }
		// From slGetFeatureRequirements(DLSS-G); false when the runtime does not say.
		bool DLSSGVSyncOffRequired() const { return dlssgVSyncOffRequired_.load(std::memory_order_relaxed); }
		void ConfigureXeFG(XeFGOptions options)
		{
			options = SanitizeXeFG(options);
			xefgOptions_.store(options.generatedFrames | (options.experimentalMFG ? 0x100u : 0u));
		}
		XeFGOptions XeFGConfiguration() const
		{
			const auto value = xefgOptions_.load();
			return {(value & 0x100u) != 0, value & 0xffu};
		}
		const std::string& ProviderStatus() const { return providerStatus_; }
		bool GenerationActive() const { return Ready() && !startupTransport_ && (provider_ == FrameGenerationProvider::XeFG ? xefg_.Snapshot().enabled : session_.Snapshot().GenerationActive()); }
		unsigned EffectiveMultiplier() const { return startupTransport_ ? 1 : provider_ == FrameGenerationProvider::XeFG ? xefg_.Snapshot().generatedFrames+1 : session_.Snapshot().options.numFramesToGenerate + 1; }
		std::uint32_t FrameIndex() const { return startupTransport_ ? transportFrame_ : provider_ == FrameGenerationProvider::XeFG ? xefg_.Snapshot().frameId : nvidiaNeedsPresent_ ? xefg_.Snapshot().frameId + 1 : session_.Snapshot().frameIndex; }
		bool ApplyProviderSwitch(SwapChain& wrapper);
		// A live switch to NVIDIA would be refused: NR initialized NGX before
		// Streamline in this session. Saving NVIDIA and restarting resolves it.
		bool NvidiaSwitchBlocked() const;
		bool NvidiaServicesReady() const { return Ready() && nvidiaInitialized_ && !startupTransport_; }
		bool StartupTransport() const { return startupTransport_; }
		// CPU time of one host Present split into its phases: input/output
		// recording, the presenter's own Present, and retirement/replacement.
		void ObservePresentTiming(double prepareMs, double presentMs, double finishMs);
		// After AfterPresent: a switch will replace the presenter at this boundary.
		bool ProviderSwitchPending() const { return providerSwitchPending_; }
		void ConfigureMFGUnlock(bool requested) { if (!attempted_) { mfgUnlock_.Configure(requested); } }
		void ConfigureRTX30(bool legacyRuntime, bool emulateOnRTX40)
		{
			if (attempted_) { return; }
			rtx30LegacyDLSSG_ = legacyRuntime;
			mfgUnlock_.ConfigureEmulation(emulateOnRTX40);
		}
		const MFGSnapshot& MFGState() const { return mfgUnlock_.Snapshot(); }
		HRESULT CreateSwapChain(IDXGIFactory* a_factory, ID3D11Device* a_device,
			const DXGI_SWAP_CHAIN_DESC& a_desc, const std::filesystem::path& a_directory,
			IDXGISwapChain** a_result);
		bool Prepare(const sl::Constants& a_constants, ID3D11Texture2D* a_motion,
			ID3D11Texture2D* a_depth, ID3D11Texture2D* a_ui, ID3D11Texture2D* a_hudless,
			FrameExtent a_renderExtent, UINT a_displayWidth, UINT a_displayHeight, bool a_neuralEligible = true);
		void ConfigureNeuralRendering(NeuralOptions a_options);
		// Freeze NR settings before DLSS, optionally replace its D3D11 input, and
		// return a shared history reset. A null camera skips the early NR stage.
		bool EvaluateNeuralBeforeUpscaling(const NeuralOptions& options, const sl::Constants* camera,
			bool eligible, ID3D11Texture2D* color, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
			FrameExtent renderExtent, bool& reset);
		bool EvaluateNeuralWorld(const NeuralOptions& options, const sl::Constants* camera,
			bool eligible, ID3D11Texture2D* color, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
			FrameExtent guideExtent, FrameExtent colorExtent, bool& reset);
		NeuralOptions NeuralConfiguration() const;
		// Session-only NR comparison, applied live from the menu.
		void ConfigureNeuralCompare(NeuralRendering::CompareSettings a_compare);
		NeuralRendering::CompareSettings NeuralCompare() const;
		int NeuralComputeCapability() const;
		NeuralSnapshot NeuralState() const;
		bool ConfigureReflex(sl::ReflexMode a_mode)
		{
			if (!ValidReflexMode(a_mode)) { return false; }
			reflexMode_.store(a_mode, std::memory_order_relaxed);
			return true;
		}
		sl::ReflexMode ReflexConfiguration() const { return reflexMode_.load(std::memory_order_relaxed); }
		// A<->B and B<->C change UI recomposition, which Streamline recreates for;
		// A<->C changes only Lite blending, so DLSS-G is released for one Present.
		void ConfigurePreset(DlssgPreset a_preset);
		DlssgPreset PresetConfiguration() const { return preset_.load(std::memory_order_relaxed); }
		bool UIRecompositionConfiguration() const { return UsesUIRecomposition(PresetConfiguration()); }
		void ConfigureXeFGFrameTime(bool a_enabled) { xefgFrameTime_.store(a_enabled, std::memory_order_relaxed); }
		bool XeFGFrameTimeConfiguration() const { return xefgFrameTime_.load(std::memory_order_relaxed); }
		void ConfigureXeFGSceneChangeThreshold(float a_threshold)
		{
			xefgSceneChange_.store(SanitizeXeFGSceneChange(a_threshold), std::memory_order_relaxed);
		}
		float XeFGSceneChangeThresholdConfiguration() const { return xefgSceneChange_.load(std::memory_order_relaxed); }
		// Lab diagnostics, not saved: present only generated frames (Intel and NVIDIA), or tag them (Intel).
		void ConfigureXeFGDebugView(bool a_onlyGenerated, bool a_tagGenerated)
		{
			xefgOnlyGenerated_.store(a_onlyGenerated, std::memory_order_relaxed);
			xefgTagGenerated_.store(a_tagGenerated, std::memory_order_relaxed);
		}
		bool XeFGOnlyGenerated() const { return xefgOnlyGenerated_.load(std::memory_order_relaxed); }
		// Lab A/B, not saved: after Present, queue a D3D11 GPU wait for Intel's
		// input copies instead of draining every host lane on the CPU.
		void ConfigureXeFGGpuInputWait(bool a_enabled) { xefgGpuInputWait_.store(a_enabled, std::memory_order_relaxed); }
		bool XeFGGpuInputWait() const { return xefgGpuInputWait_.load(std::memory_order_relaxed); }
		// Lab A/B, not saved: NVIDIA's next frame waits for frame generation's
		// inputs only where TRP first writes a shared texture, not at its start.
		void ConfigureNvidiaOverlapNextFrame(bool a_enabled) { nvidiaOverlapNextFrame_.store(a_enabled, std::memory_order_relaxed); }
		bool NvidiaOverlapNextFrame() const { return nvidiaOverlapNextFrame_.load(std::memory_order_relaxed); }
		bool XeFGTagGenerated() const { return xefgTagGenerated_.load(std::memory_order_relaxed); }
		bool ConfigureOutputFPSLimit(int a_fps)
		{
			if (a_fps < 0 || a_fps > 1000) { return false; }
			outputFPSLimit_.store(a_fps, std::memory_order_relaxed);
			return true;
		}
		void ConfigureFrameLimit(FrameLimitMode a_mode) { frameLimit_.store(static_cast<int>(a_mode), std::memory_order_relaxed); }
		// Steady's current rendered-frame cap, 0 while measuring or off.
		float SteadyRenderedFPS() const { return steadyRenderedFPS_.load(std::memory_order_relaxed); }
		// Applied at the next present boundary (UpdateHDRRequest); calibration applies live.
		void ConfigureHDROutput(const HDROutput::Settings& a_settings)
		{
			std::scoped_lock lock(hdrMutex_); hdrSettings_ = HDROutput::Sanitize(a_settings);
		}
		HDROutput::Settings HDROutputConfiguration() const { std::scoped_lock lock(hdrMutex_); return hdrSettings_; }
		HDROutputState HDRState() const { std::scoped_lock lock(hdrMutex_); return hdrState_; }
		// Native swapchain format for a game-facing format. Renderer-owned HDR
		// keeps 8-bit producers unchanged and presents RGB10A2 underneath.
		DXGI_FORMAT NativeFormat(DXGI_FORMAT a_gameFormat) const
		{
			return hdrNative_ && HDROutputEligibleFormat(a_gameFormat) ? HDROutputPass::kOutputFormat : PresentationFormat(a_gameFormat);
		}
		bool HDRNative() const { return hdrNative_; }
		void ApplyNativeColorSpace();
		// Render thread, between frames. Applies a changed HDR output setting and
		// returns true when the native swapchain must first be reallocated as
		// RGB10A2 (SwapChain::StartHDROutput). Turning HDR off keeps that format.
		bool UpdateHDRRequest(DXGI_FORMAT a_gameFormat);
		void SetHDRNative(bool a_native) { hdrNative_ = a_native; }
		void FinishHDRStart(HRESULT a_reallocated, bool a_generation);
		bool Enabled() const { return enabled_; }
		void SetEnabled(bool a_enabled) { enabled_ = a_enabled; }
		void SetTransitionBlocked(bool a_blocked);
		bool TransitionBlocked() const { return transitionBlocked_.load(std::memory_order_acquire); }
		bool ConfigureGeneration(GenerationRequest a_request)
		{
			if (!ValidGenerationRequest(a_request)) { return false; }
			std::scoped_lock lock(generationMutex_); generationRequest_ = a_request;
			return true;
		}
		GenerationRequest GenerationConfiguration() const
		{
			std::scoped_lock lock(generationMutex_); return generationRequest_;
		}
		bool Ready() const { return ready_ && SUCCEEDED(fault_); }
		bool Quiesce(RetirementPurpose purpose = RetirementPurpose::Resize);
		const SessionSnapshot& Snapshot() const { return session_.Snapshot(); }
		PresentationFeedbackSnapshot PresentationFeedback() const;
		RuntimeDiagnosticsSnapshot RuntimeDiagnosticState() const { return runtimeDiagnostics_.Snapshot(); }
		bool FrameGenerationOverrideReported() const { return runtimeDiagnostics_.FrameGenerationOverrideObserved(); }
		bool FrameGenerationUnsupportedReported() const { return runtimeDiagnostics_.FrameGenerationUnsupportedObserved(); }
		const std::string& Status() const { return status_; }
		ID3D11Device* Device11() const { return device11_.Get(); }
		ID3D12CommandQueue* Queue() const { return queue_.Get(); }
		Interop& Transport() { return interop_; }
		XeSSUpscaler& XeSS() { return xess_; }
		// Session pipeline library for XeSS initialization, opened on first use.
		ID3D12PipelineLibrary* XeSSPipelines() { return IntelPipelines(xessPipelines_, "xess", xess_.Module()); }
		const XeSSUpscaler& XeSS() const { return xess_; }
		ID3D12Device* Device12() const { return device12_.Get(); }
		bool NvidiaAdapter() const { return adapterVendor_ == 0x10DE; }
		HRESULT BeforePresent(ID3D12Resource* a_source, ID3D12Resource* a_destination, DXGI_COLOR_SPACE_TYPE a_colorSpace);
		HRESULT AfterPresent(HRESULT a_result);
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
		HRESULT RequestOutputCapture(std::uint64_t id) { return outputCapture_.Request(id); }
		HRESULT PollOutputCapture(TheosRenderPipeline::SourceOutputCapture::Result& result) { return outputCapture_.Poll(result); }
#endif
		bool ResumeAfterResize();
		bool ResumeAfterUpscalerReplacement();
	private:
		friend class SwapChain; // Transport failures use the same first-error latch.
		Backend() = default;
		bool EnterStartupTransport();
		bool startupTransport_{}, presenterAttempted_{};
		std::uint32_t transportFrame_{};
		std::string startupFailure_;
		bool Load(const std::filesystem::path& a_directory);
		bool InitializeNvidia(const std::filesystem::path& directory);
		bool UpgradeNvidiaFactory();
		void ReadDLSSGRequirements();
		bool PreflightProviderSwitch();
		HRESULT CreatePresenter(FrameGenerationProvider provider, IDXGISwapChain** result);
		bool Check(sl::Result a_result, const char* a_operation);
		// a_source tags the log line; Intel SDK failures use XeFG or XeSS.
		bool Check(HRESULT a_result, const char* a_operation, const char* a_source = "SourceDLSSG");
		bool CheckSession(bool a_result);
		bool CheckXeFG(HRESULT result);
		// An optional guide's allocation failure returns false without a fault.
		bool EnsureGuide(ID3D11Texture2D* a_source, SharedTexture& a_pair,
			DXGI_FORMAT a_format = DXGI_FORMAT_UNKNOWN, FrameExtent a_extent = {}, bool a_optional = false);
		bool CopyDepth(ID3D11Texture2D* a_depth);
		void ReleaseGuides();
		bool RecreateNeuralIfNeeded(const NeuralOptions& options);
		bool FailNeuralRecording();
		const char* NeuralUnavailableReason(const NeuralOptions& options);
		void RecordPresentationFeedback(std::uint32_t a_presentCount,
			std::int64_t a_observedQpc, std::int64_t a_syncQpc);
		void ResetPresentationFeedback();
		void RecordScreenshot(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_output, DXGI_COLOR_SPACE_TYPE a_colorSpace) noexcept;
		void FinishScreenshot() noexcept;
		static void StreamlineLogCallback(sl::LogType a_type, const char* a_message);
		void PollDisplayHDR(bool a_force);
		// PQ is signalled: RGB10A2 native buffers, HDR output on and Windows HDR on.
		bool HDRSignalled() const { return hdrNative_ && hdrRequested_ && hdrDisplay_; }
		const char* HDRDisplayReason(bool a_known) const;
		void RefreshSDRWhite();
		bool EnsureHDRTargets(UINT a_width, UINT a_height);
		HRESULT RecordOutput(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_source, ID3D12Resource* a_destination,
			bool a_prepared, DXGI_COLOR_SPACE_TYPE& a_colorSpace);
		HMODULE interposer_{};
		std::array<HMODULE, 6> runtimeModules_{}; // Retained with this process-resident owner.
		PFun_slInit* init_{};
		PFun_slSetFeatureLoaded* loadFeature_{};
		PFun_slSetD3DDevice* setDevice_{};
		PFun_slIsFeatureSupported* supported_{};
		PFun_slGetFeatureFunction* featureFunction_{};
		PFun_slUpgradeInterface* upgrade_{};
		PFun_slGetFeatureRequirements* requirements_{}; // Optional: diagnostics and VSync policy only.
		SessionAPI api_{};
		Session session_;
		XeFGPresenter xefg_;
		std::atomic<bool> xefgTensorRequested_{};
		std::atomic<std::uint32_t> xefgOptions_{1};
		XeSSUpscaler xess_;
		UINT adapterVendor_{};
		bool nvidiaInitialized_{}, nvidiaSessionStarted_{};
		FrameGenerationProvider provider_{FrameGenerationProvider::NVIDIA};
		std::atomic<FrameGenerationProvider> requestedProvider_{FrameGenerationProvider::NVIDIA};
		std::atomic<VSyncMode> vsyncMode_{VSyncMode::Auto};
		mutable std::mutex vsyncMutex_;
		VSyncObservation vsyncState_;
		Microsoft::WRL::ComPtr<IDXGIFactory> nativeFactory_, streamlineFactory_;
		DXGI_SWAP_CHAIN_DESC presenterDesc_{};
		std::string providerStatus_{"NVIDIA presenter"};
		bool providerSwitchPending_{}, cameraAvailable_{}, recreateXeFG_{}, nvidiaNeedsPresent_{};
		sl::Result reportedStateQueryResult_{ sl::Result::eOk };
		sl::Result reportedOptionsResult_{ sl::Result::eOk };
		std::atomic<bool> dlssgVSyncOffRequired_{};
		int reportedVSyncHint_{-1};
		MFGUnlock mfgUnlock_;
		bool rtx30LegacyDLSSG_{};
		std::atomic<sl::ReflexMode> reflexMode_{ sl::ReflexMode::eLowLatency };
		std::atomic<DlssgPreset> preset_{ DlssgPreset::B };
		std::atomic<bool> releaseForPreset_{};
		std::atomic<bool> xefgFrameTime_{ true }, xefgOnlyGenerated_{}, xefgTagGenerated_{}, xefgGpuInputWait_{}, xefgFlipMetering_{true};
		std::atomic<float> xefgSceneChange_{ XeFGDefaultSceneChangeThreshold };
		std::atomic<bool> nvidiaOverlapNextFrame_{};
		bool nvidiaOverlapLogged_{}; // Present thread.
		std::uint64_t frameGenerationTimingKey_{ ~0ull }; // Present thread; the configuration being timed.
		// Per-window CPU samples for the input-reuse A/B; logged and cleared every 600 presents.
		std::vector<float> xefgReuseMs_, xefgSleepMs_;
		bool xefgReuseWindowGpu_{};
		SlowPresentGate slowPresent_;
		ID3D12PipelineLibrary* IntelPipelines(PipelineCache& cache, const char* name, HMODULE runtime);
		// Each Intel SDK copy keeps its own pipeline cache.
		PipelineCache xefgPipelines_, xefgTensorPipelines_, xessPipelines_;
		PipelineCache& XeFGPipelineCache() { return xefg_.TensorWanted() ? xefgTensorPipelines_ : xefgPipelines_; }
		XeFGSettleGate xefgSettle_;
		std::chrono::steady_clock::time_point lastHostFrame_{};
		UINT adapterDevice_{};
		LARGE_INTEGER adapterDriver_{};
		std::uint64_t presenterPresents_{}; // Since the current presenter was created.
		bool presenterReplaced_{}; // ApplyProviderSwitch replaced it during this Present.
		std::atomic<int> outputFPSLimit_{ 0 };
		std::atomic<int> frameLimit_{ 0 };
		std::atomic<float> steadyRenderedFPS_{ 0 };
		// Render thread only. The output cap Present hands the limiters this frame.
		SteadyPacing steadyPacing_;
		std::chrono::steady_clock::time_point steadyLast_{};
		int frameOutputLimit_{ 0 };
		int UpdateFrameLimit();
		mutable std::mutex presentationFeedbackMutex_;
		PresentationFeedbackTracker presentationFeedback_;
		std::uint64_t presentationQueryFailures_{};
		std::int64_t presentationQpcFrequency_{};
		std::uint64_t neuralTimestampFrequency_{};
		bool timerReported_{}; // Stage timer availability was logged.
		RuntimeDiagnostics runtimeDiagnostics_;
		mutable std::mutex generationMutex_;
		GenerationRequest generationRequest_;
		Interop interop_;
		DeviceLossDiagnostics deviceLoss_;
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
		TheosRenderPipeline::SourceOutputCapture outputCapture_;
#endif
		Microsoft::WRL::ComPtr<ID3D11Device> device11_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12Device> upgradedDevice12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
		Microsoft::WRL::ComPtr<IDXGISwapChain> retainedNative_;
		SharedTexture motion_, depth_, ui_, hudless_, earlyNeuralColor_, objectMask_;
        bool objectMaskCurrent_{};
        bool objectMaskRefused_{}; // Its shared texture could not be created; normal NR for the session.
        bool CopyObjectMask(FrameExtent extent);
#if !defined(TRP_NO_NEURAL_RENDERING)
		std::unique_ptr<NeuralPass> neuralPass_;
		NeuralRuntimeAvailability neuralAvailability_; // Protected by neuralMutex_.
		const char* neuralReportedUnavailable_{};
		// Runtimes refused for this GPU this session, and the GPU's CUDA compute capability (-1 = not queried).
		// Each refused runtime with the note shown while it stays selected.
		std::vector<std::pair<std::filesystem::path, std::string>> neuralRefusedRuntimes_; // Protected by neuralMutex_.
		int neuralComputeCapability_{ -1 };
		std::filesystem::path neuralLoggedRuntime_; // Last runtime Automatic or a fallback chose.
		// Requires neuralMutex_. Picks Automatic's runtime for this GPU once the
		// device exists, and replaces a refused runtime with Automatic's choice.
		// Returns the refusal note while it applies, or nullptr.
		const std::string* SelectNeuralRuntime(NeuralOptions& options);
		// Requires neuralMutex_. Refuses a runtime for the session and switches.
		void RefuseNeuralRuntime(const std::filesystem::path& runtime, const std::string& reason, std::string_view advice);
		// After a failed NR Record: true when NGX refused to create the feature,
		// so nothing was recorded and NR moves to the next runtime instead of failing.
		bool RefuseNeuralCreation(const std::filesystem::path& runtime);
#endif
		std::unique_ptr<HDRPass> hdrPass_;
		// Renderer-owned HDR output (render thread except the guarded settings/state).
		mutable std::mutex hdrMutex_;
		HDROutput::Settings hdrSettings_;
		HDROutputState hdrState_;
		std::unique_ptr<HDROutputPass> hdrOutputPass_;
		Microsoft::WRL::ComPtr<IDXGIFactory1> hdrFactory_;
		HWND window_{};
		HMONITOR hdrMonitor_{};
		std::uint32_t hdrPollCountdown_{};
		std::uint64_t hdrTimestampFrequency_{};
		std::array<wchar_t, 32> hdrDeviceName_{}, hdrSDRDevice_{};
		float hdrSDRWhiteNits_{};
		std::uint32_t hdrSDRPolls_{};
		bool hdrNative_{}, hdrRequested_{}, hdrDisplay_{}, hdrFrameTagged_{}, hdrColorSpaceApplied_{};
		// Replaces a ReShade screenshot of the UI-only source runtime with the
		// final real frame. Failures keep ReShade's file and never fault rendering.
		FinalFrameCapture screenshotCapture_;
		ScreenshotWorker screenshotWriter_;
		std::string screenshotPath_;
		EngineScreenshot::Request screenshotPhoto_;  // a flag-only request: the writer finds the photo
		int screenshotQuality_{};
		bool screenshotRecorded_{}, screenshotFaultLogged_{};
		NeuralHistory neuralHistory_;
		mutable std::mutex neuralMutex_;
		NeuralOptions neuralOptions_;
		NeuralRendering::CompareSettings neuralCompare_; // Protected by neuralMutex_.
		NeuralOptions frameNeuralOptions_;
		NeuralSnapshot neuralSnapshot_;
		sl::Constants frameConstants_{};
		bool neuralEligible_{};
		bool frameNeuralReset_{};
		bool frameNeuralModelChanged_{};
		bool neuralFrameBegun_{}, neuralEvaluatedEarly_{};
		Microsoft::WRL::ComPtr<ID3D11Texture2D> uiSource_;
		D3D11FrameCopy::Depth depthCopy_;
		std::filesystem::path directory_;
		static constexpr std::uint32_t TransitionWarmupPresents = 3;
		std::atomic_bool transitionBlocked_{ true };
		std::atomic<std::uint32_t> transitionWarmupPresents_{};
		bool attempted_{ false }, ready_{ false }, enabled_{ false };
		HRESULT fault_{ S_OK };
		std::string status_{ "not requested" };
	};
}
