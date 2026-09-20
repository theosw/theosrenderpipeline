#pragma once

#include "NeuralRenderingFeatureSession.h"
#include "SourceDLSSGInterop.h"
#include "SourceDLSSGNeuralState.h"
#include "SourceDLSSGNeuralResolve.h"
#include "SourceDLSSGNeuralTelemetry.h"
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
			std::uint64_t timestampFrequency = 0);
		ID3D12Resource* Composed() const { return composed_.Get(); }
		ID3D12Resource* Corrected() const { return corrected_.Get(); }
		NeuralTelemetrySnapshot Telemetry() const { return telemetry_.Snapshot(); }
		void RetireTelemetry();
		bool NeedsRecreation(const NeuralOptions& options, UINT guideWidth = 0, UINT guideHeight = 0) const
		{
			return feature_.IsInitialized() && (runtimePath_ != options.runtimePath || beforeUpscaling_ != options.beforeUpscaling || worldOnly_ != options.WorldOnly() || passes_ != options.passes ||
				!NeuralRendering::SameReconstructionResources(reconstruction_, options.reconstruction) ||
				((guideWidth || guideHeight) && (guideWidth != guideWidth_ || guideHeight != guideHeight_)));
		}
		const std::string& Status() const { return status_; }
	private:
		bool Initialize(ID3D12Device* device, const NeuralOptions& options,
			ID3D12Resource* motion, ID3D12Resource* hudless, ID3D12Resource* composed);
		void InitializeTelemetry(ID3D12Device* device, std::uint64_t timestampFrequency);
		void HarvestTelemetry(std::size_t slot);
		struct PendingTiming
		{
			std::uint64_t evaluation{};
			std::uint64_t cpuRecordNanoseconds{};
			bool pending{};
		};
		NeuralRendering::FeatureSession feature_;
		// Each session owns a runtime reference, including failed creation work.
		NeuralRendering::FeatureSession secondFeature_;
		Microsoft::WRL::ComPtr<ID3D12Resource> secondOutput_;
		Microsoft::WRL::ComPtr<ID3D12Resource> corrected_, composed_;
		Microsoft::WRL::ComPtr<ID3D12Resource> encoded_, workColor_, workOutput_, residual_;
		Microsoft::WRL::ComPtr<ID3D12Resource> packedMotion_, packedDepth_, packedUI_;
		NeuralResolveKernels resolve_;
		bool fusedColor_{};
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
	};
}
