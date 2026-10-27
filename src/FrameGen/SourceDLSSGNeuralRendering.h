#pragma once

#include "NeuralRenderingFeatureSession.h"
#include "SourceDLSSGInterop.h"
#include "SourceDLSSGNeuralState.h"
#include "SourceDLSSGNeuralResolve.h"
#include "SourceDLSSGNeuralTelemetry.h"
#include "NeuralObjectBlend.h"
#include "NeuralObjectControl.h"
#include "NeuralModelRate.h"
#include <chrono>
#include <array>

namespace TheosRenderPipeline::SourceDLSSG
{
	// Render-thread owner. Destroy ONLY after Interop::Drain proves retirement.
	// A recording failure is fatal to the backend: unknown NGX commands must not
	// be submitted or followed by a speculative fallback using guessed states.
	class NeuralPass final
	{
	public:
		~NeuralPass();
		bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
			const NeuralOptions& options, bool reset, bool depthInverted,
			float motionScaleX, float motionScaleY,
			ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* ui,
			ID3D12Resource* hudless, ID3D12Resource* composed,
			std::uint64_t timestampFrequency = 0, const D3D12StageMarks& marks = {}, ID3D12Resource* objectMask = nullptr,
            std::array<float,2> objectMaskGuideOffset = {},
			std::array<float,2> colorJitter = {});
		ID3D12Resource* Composed() const { return composed_.Get(); }
		ID3D12Resource* Corrected() const { return corrected_.Get(); }
		NeuralTelemetrySnapshot Telemetry() const { return telemetry_.Snapshot(); }
		void RetireTelemetry();
		NeuralRendering::RuntimeBuild RetainedRuntimeBuild(const std::filesystem::path& request) const
		{
			// runtimePath_ is published only after every requested feature initializes.
			// Changed spellings take the normal verification path; do no filesystem I/O here.
			return request.is_absolute() && runtimePath_ == request && feature_.IsInitialized() ? feature_.Build() :
				NeuralRendering::RuntimeBuild::Unknown;
		}
		bool NeedsRecreation(const NeuralOptions& options, UINT guideWidth = 0, UINT guideHeight = 0) const
		{
			return NeedsResourceRecreation(options, guideWidth, guideHeight) || ModelsChanged(options);
		}
		bool ModelChangePending(const NeuralOptions& options) const { return ModelsChanged(options); }
		bool NeedsResourceRecreation(const NeuralOptions& options, UINT guideWidth = 0, UINT guideHeight = 0) const
		{
			auto resources = reconstruction_;
			resources.preset = options.reconstruction.preset;
			return feature_.IsInitialized() && (runtimePath_ != options.runtimePath || beforeUpscaling_ != options.beforeUpscaling || worldOnly_ != options.WorldOnly() || passes_ != options.passes ||
				!NeuralRendering::SameReconstructionResources(resources, options.reconstruction) ||
				ExtraPassesChanged(options) ||
				((guideWidth || guideHeight) && (guideWidth != guideWidth_ || guideHeight != guideHeight_)));
		}
		// No list may be open. Backend drains first, just as for a full rebuild.
		bool RetireChangedModels(const NeuralOptions& options);
		NeuralRendering::RuntimeBuild Build() const { return feature_.Build(); }
		std::array<int, NeuralRendering::kMaxNeuralPasses> SubmittedPresets() const
		{
			std::array<int, NeuralRendering::kMaxNeuralPasses> result{};
			result[0] = feature_.SubmittedPreset();
			for (std::size_t i = 0; i < extra_.size(); ++i) { result[i + 1] = extra_[i].feature.SubmittedPreset(); }
			return result;
		}
		const std::string& Status() const { return status_; }
		// NGX refused to create Pass 1 with this runtime. Initialization then stops
		// before Record writes to the frame's list. A refused later pass is not a
		// runtime refusal: the passes before it keep running (see PassNote).
		std::uint32_t RefusedCreation() const { return feature_.RefusedCreation(); }
		// Passes whose feature was created on an earlier frame; at most these run.
		int ReadyPasses() const { return readyPasses_; }
		// Why fewer passes run than requested for the rest of this pass's life, or empty.
		const std::string& PassNote() const { return passNote_; }
	private:
		bool ModelsChanged(const NeuralOptions& options) const
		{
			if (!corrected_) { return false; }
			if (reconstruction_.preset != options.reconstruction.preset) { return true; }
			for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(options.passes); ++i) {
				if (extra_[i].settings.preset != options.EffectiveExtra(i).preset) { return true; }
			}
			return false;
		}
		bool ExtraPassesChanged(const NeuralOptions& options) const
		{
			for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(options.passes) && i < extra_.size(); ++i) {
				const auto wanted = options.EffectiveExtra(i);
				if (extra_[i].settings.inputScale != wanted.inputScale) { return true; }
			}
			return false;
		}
		bool Initialize(ID3D12Device* device, const NeuralOptions& options,
			ID3D12Resource* motion, ID3D12Resource* hudless, ID3D12Resource* composed);
		// pass is 1 for Pass 2 up to 4 for Pass 5. source is the previous pass's
		// result on Pass 1's grid: Pass 1's writable output for Pass 2, otherwise
		// an earlier FinishExtra's result in COMMON.
		bool RecordExtra(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot, unsigned pass,
			const NeuralOptions& options, const NeuralRendering::FeatureSession::EvaluationInput& first,
			ID3D12Resource* source, const ResolveConstants& constants, ID3D12Resource* motion, ID3D12Resource* depth,
			ID3D12Resource* ui, NeuralRendering::FeatureSession::EvaluationInput& evaluation);
		// With directResult, the pass's result on Pass 1's grid is returned in COMMON,
		// for the next pass or the resolve to read; otherwise it is copied back to
		// first.output in UAV state.
		bool FinishExtra(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot, unsigned pass,
			const NeuralRendering::FeatureSession::EvaluationInput& first, ID3D12Resource* source,
			const NeuralRendering::FeatureSession::EvaluationInput& evaluation, const ResolveConstants& originalConstants,
			ID3D12Resource** directResult);
		// Every field shown by the success status. The text is formatted only when one changes.
		struct StatusKey
		{
			UINT workWidth{}, workHeight{}, sourceWidth{}, sourceHeight{};
			int passes{};
			bool worldOnly{}, beforeUpscaling{}, producerColor{}, peripheral{}, fusionRequested{}, fusedColor{};
			NeuralRendering::ResolveMethod method{};
			struct Extra
			{
				UINT width{}, height{};
				int preset{};
				bool linked{};
				float intensity{};
				bool operator==(const Extra&) const = default;
			};
			std::array<Extra, NeuralRendering::kExtraPasses> extra{};
			NeuralRendering::PassOverride passOverride{};
			int requestedPasses{}, readyPasses{}, startablePasses{};
			std::array<bool, NeuralRendering::kMaxNeuralPasses> objectControls{};
			bool objectControlsUnavailable{};
			unsigned modelInterval{};
			bool operator==(const StatusKey&) const = default;
		};
		void PublishStatus(const StatusKey& key);
		void InitializeTelemetry(ID3D12Device* device, std::uint64_t timestampFrequency);
		void HarvestTelemetry(std::size_t slot);
		struct PendingTiming
		{
			std::uint64_t evaluation{};
			std::uint64_t cpuRecordNanoseconds{};
			bool pending{};
			bool modelRan{};
		};
		NeuralRendering::FeatureSession feature_;
		// Passes 2 to 5. Each runs the model on the previous pass's result with its
		// own feature, history and model raster, and is restored to Pass 1's grid
		// before the next pass reads it.
		struct ExtraPass
		{
			// Each session owns a runtime reference, including failed creation work.
			NeuralRendering::FeatureSession feature;
			Microsoft::WRL::ComPtr<ID3D12Resource> output, input, restored;
			Microsoft::WRL::ComPtr<ID3D12Resource> motion, depth, ui;
			NeuralRendering::ExtraPassSettings settings;
			ID3D12Resource* controlMask{}; // frame-only, owned by objectControl_
			bool historyInvalid{true};
		};
		std::array<ExtraPass, NeuralRendering::kExtraPasses> extra_;
		// Features created so far, Pass 1 included, and how many of them may run.
		int createdPasses_{}, readyPasses_{};
		// Lowered when NGX refuses a later pass's feature; fixed until recreation.
		int startablePasses_{NeuralRendering::kMaxNeuralPasses};
		std::string passNote_;
		Microsoft::WRL::ComPtr<ID3D12Resource> corrected_, composed_;
		Microsoft::WRL::ComPtr<ID3D12Resource> encoded_, workColor_, workOutput_, residual_;
		Microsoft::WRL::ComPtr<ID3D12Resource> packedMotion_, packedDepth_, packedUI_;
		NeuralResolveKernels resolve_;
        NeuralRendering::ObjectBlend objectBlend_;
        Microsoft::WRL::ComPtr<ID3D12Resource> objectResult_;
		Microsoft::WRL::ComPtr<ID3D12Resource> splitResult_; // Session-only compare
		// Freeze frame: the held scene, its depth and object mask, and a motion
		// field that is never written, so stays zero.
		Microsoft::WRL::ComPtr<ID3D12Resource> heldColor_, heldDepth_, heldMask_, stillMotion_;
		bool frozen_{};
		// Adaptive exposure: the measured white point (1x1 R32_FLOAT) and when it
		// was last updated, for a frame-rate independent adaptation speed.
		Microsoft::WRL::ComPtr<ID3D12Resource> exposure_;
		std::chrono::steady_clock::time_point exposureTime_{};
		bool exposureValid_{};
		// Broad and Detail: NR's change in stops at its grid, blurred, and scratch.
		Microsoft::WRL::ComPtr<ID3D12Resource> toneData_, toneScratch_;
		// Records the hold; returns false only on a failure that leaves the frame unusable.
		bool HoldFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, bool freeze,
			ID3D12Resource*& motion, ID3D12Resource*& depth, ID3D12Resource* hudless, ID3D12Resource*& objectMask, bool& reset);
        std::array<NeuralRendering::ObjectBlend,NeuralRendering::kMaxNeuralPasses> passObjectBlend_;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,NeuralRendering::kMaxNeuralPasses> passObjectResult_;
        NeuralRendering::ObjectControl objectControl_;
        std::array<bool,NeuralRendering::kMaxNeuralPasses> objectControlActive_{};
		bool fusedColor_{};
		// Model rate (NeuralModelRate.h). Each frame's guides alternate between
		// two textures; the change is NR's last stored result relative to its
		// input; the motion is composed for an evaluation after a skipped frame.
		// All stay COMMON between dispatches and are allocated on first use.
		bool EnsureModelRate(ID3D12Device* device, ID3D12Resource* motion, ID3D12Resource* hudless);
		NeuralRendering::ModelRateSchedule rateSchedule_;
		std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> rateGuides_;
		Microsoft::WRL::ComPtr<ID3D12Resource> rateChange_, rateMotion_;
		unsigned rateGuide_{};
		NeuralOptions rateOptions_;
		int rateEffectivePasses_{};
		std::chrono::steady_clock::time_point rateTime_{};
		// The previous captured frame's camera jitter and whether it classified objects.
		std::array<float, 2> rateJitter_{};
		bool rateMask_{};
		bool createdThisFrame_{};
		bool firstHistoryInvalid_{};
		NeuralRendering::Reconstruction reconstruction_;
		std::filesystem::path runtimePath_;
		UINT guideWidth_{}, guideHeight_{};
		bool beforeUpscaling_{};
		bool worldOnly_{};
		int passes_{1};
		Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
		std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, kCommandSlots> heaps_;
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampHeap_;
		Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback_;
		std::array<PendingTiming, kCommandSlots> pendingTiming_{};
		std::uint64_t* mappedTimestamps_{};
		std::uint64_t timestampFrequency_{};
		bool telemetryAttempted_{};
		NeuralTelemetryTracker telemetry_;
		std::string status_;
		StatusKey statusKey_{};
		std::string successStatus_;
	};
}
