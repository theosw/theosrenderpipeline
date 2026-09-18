#pragma once

#include "SourceDLSSGInterop.h"
#include "D3D11FrameCopy.h"
#include "SourceDLSSGSession.h"
#include "SourceDLSSGNeuralState.h"
#if !defined(TRP_NO_NEURAL_RENDERING)
#include "SourceDLSSGNeuralRendering.h"
#include "SourceDLSSGNeuralAvailability.h"
#endif
#include "SourceDLSSGHDR.h"
#include "SourceDLSSGMFG.h"
#include "SourceDLSSGPresentationFeedback.h"
#include "SourceDLSSGRuntimeDiagnostics.h"
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
#include "FrameGen/SourceOutputCapture.h"
#endif
#include <dxgi1_5.h>
#include <filesystem>
#include <array>
#include <string>
#include <mutex>
#include <atomic>

namespace TheosRenderPipeline::SourceDLSSG
{
	class SwapChain;
	// Process-resident Streamline owner. Rejects competing runtime owners
	// and never unloads plugin code while DXGI proxy objects could still call it.
	class Backend final
	{
	public:
		static Backend& Get();
		void ConfigureMFGUnlock(bool requested) { if (!attempted_) { mfgUnlock_.Configure(requested); } }
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
		NeuralSnapshot NeuralState() const;
		bool ConfigureReflex(sl::ReflexMode a_mode)
		{
			if (!ValidReflexMode(a_mode)) { return false; }
			reflexMode_.store(a_mode, std::memory_order_relaxed);
			return true;
		}
		sl::ReflexMode ReflexConfiguration() const { return reflexMode_.load(std::memory_order_relaxed); }
		bool ConfigureOutputFPSLimit(int a_fps)
		{
			if (a_fps < 0 || a_fps > 1000) { return false; }
			outputFPSLimit_.store(a_fps, std::memory_order_relaxed);
			return true;
		}
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
		bool Quiesce();
		const SessionSnapshot& Snapshot() const { return session_.Snapshot(); }
		PresentationFeedbackSnapshot PresentationFeedback() const;
		RuntimeDiagnosticsSnapshot RuntimeDiagnosticState() const { return runtimeDiagnostics_.Snapshot(); }
		const std::string& Status() const { return status_; }
		ID3D11Device* Device11() const { return device11_.Get(); }
		ID3D12CommandQueue* Queue() const { return queue_.Get(); }
		Interop& Transport() { return interop_; }
		HRESULT BeforePresent(ID3D12Resource* a_source, ID3D12Resource* a_destination);
		HRESULT AfterPresent(HRESULT a_result);
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
		HRESULT RequestOutputCapture(std::uint64_t id) { return outputCapture_.Request(id); }
		HRESULT PollOutputCapture(TheosRenderPipeline::SourceOutputCapture::Result& result) { return outputCapture_.Poll(result); }
#endif
		bool ResumeAfterResize();
	private:
		friend class SwapChain; // Transport failures use the same first-error latch.
		Backend() = default;
		bool Load(const std::filesystem::path& a_directory);
		bool Check(sl::Result a_result, const char* a_operation);
		bool Check(HRESULT a_result, const char* a_operation);
		bool CheckSession(bool a_result);
		bool EnsureGuide(ID3D11Texture2D* a_source, SharedTexture& a_pair,
			DXGI_FORMAT a_format = DXGI_FORMAT_UNKNOWN, FrameExtent a_extent = {});
		bool CopyDepth(ID3D11Texture2D* a_depth);
		void ReleaseGuides();
		bool RecreateNeuralIfNeeded(const NeuralOptions& options);
		bool FailNeuralRecording();
		const char* NeuralUnavailableReason(const NeuralOptions& options);
		void RecordPresentationFeedback(std::uint32_t a_presentCount,
			std::int64_t a_observedQpc, std::int64_t a_syncQpc);
		void ResetPresentationFeedback();
		static void StreamlineLogCallback(sl::LogType a_type, const char* a_message);
		HMODULE interposer_{};
		std::array<HMODULE, 6> runtimeModules_{}; // Retained with this process-resident owner.
		PFun_slInit* init_{};
		PFun_slSetFeatureLoaded* loadFeature_{};
		PFun_slSetD3DDevice* setDevice_{};
		PFun_slIsFeatureSupported* supported_{};
		PFun_slGetFeatureFunction* featureFunction_{};
		PFun_slUpgradeInterface* upgrade_{};
		SessionAPI api_{};
		Session session_;
		sl::Result reportedStateQueryResult_{ sl::Result::eOk };
		MFGUnlock mfgUnlock_;
		std::atomic<sl::ReflexMode> reflexMode_{ sl::ReflexMode::eLowLatency };
		std::atomic<int> outputFPSLimit_{ 0 };
		mutable std::mutex presentationFeedbackMutex_;
		PresentationFeedbackTracker presentationFeedback_;
		std::uint64_t presentationQueryFailures_{};
		std::int64_t presentationQpcFrequency_{};
		std::uint64_t neuralTimestampFrequency_{};
		RuntimeDiagnostics runtimeDiagnostics_;
		mutable std::mutex generationMutex_;
		GenerationRequest generationRequest_;
		Interop interop_;
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
		TheosRenderPipeline::SourceOutputCapture outputCapture_;
#endif
		Microsoft::WRL::ComPtr<ID3D11Device> device11_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12Device> upgradedDevice12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
		Microsoft::WRL::ComPtr<IDXGISwapChain> retainedNative_;
		SharedTexture motion_, depth_, ui_, hudless_, earlyNeuralColor_;
#if !defined(TRP_NO_NEURAL_RENDERING)
		std::unique_ptr<NeuralPass> neuralPass_;
		NeuralRuntimeAvailability neuralAvailability_; // Protected by neuralMutex_.
		const char* neuralReportedUnavailable_{};
#endif
		std::unique_ptr<HDRPass> hdrPass_;
		NeuralHistory neuralHistory_;
		mutable std::mutex neuralMutex_;
		NeuralOptions neuralOptions_;
		NeuralOptions frameNeuralOptions_;
		NeuralSnapshot neuralSnapshot_;
		sl::Constants frameConstants_{};
		bool neuralEligible_{};
		bool frameNeuralReset_{};
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
