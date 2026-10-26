#pragma once
#include "SourceDLSSGInterop.h"
#include "NeuralRenderingPassSettings.h"
#include <array>
#include <span>

namespace TheosRenderPipeline::SourceDLSSG
{
	enum class ResolveKernel : unsigned { Downsample, Residual, Ratio, PackDepth, PackMotion, PrepareColor, PackGuides, ResizeColor, RestoreSecond, Split, MeasureExposure, PrepareTone, FilterTone,
		CaptureGuides, ComposeMotion, StoreChange, Reproject };
	struct ResolveConstants
	{
		UINT sourceWidth{}, sourceHeight{}, targetWidth{}, targetHeight{};
		UINT sourceIsBGRA{}, mode{}, passthrough{ 1 }, producerColor{};
		UINT workWidth{}, workHeight{};
		float transferStrength{ 1 }, colourStrength{ 1 }, maxRatio{ 2 }, whitePoint{ 1 };
		UINT peripheral{}, encodedFormat{}; // 0 float32, 1 float16, 2 UNORM8 intermediate
		UINT guideWidth{}, guideHeight{};
		float motionScaleX{ 1 }, motionScaleY{ 1 };
		UINT splitX{};
		float exposureBias{}, exposureRate{ 1 };
		UINT adaptiveExposure{}, exposureReset{};
		float jitterX{}, jitterY{};
		float broad{ 1 }, detail{ 1 };
		UINT toneBands{};
		// Model rate. Camera jitter of this frame and the previous one in render
		// (guide) pixels; guidesJittered when the colour raster is the steady
		// upscaled output and only the guides are jittered.
		float cameraJitterX{}, cameraJitterY{}, previousJitterX{}, previousJitterY{};
		UINT guidesJittered{}, depthInverted{};
		// Both frames classified objects, so their groups can be compared.
		UINT categories{};
		// The stored change's floor: multiplicative above it, additive below.
		float changeFloor{};
	};
	static_assert(sizeof(ResolveConstants) == 152);
	// All resources enter and leave COMMON. Slot retirement is the caller's
	// responsibility. Each dispatch in a frame needs a distinct stage index.
	class NeuralResolveKernels
	{
	public:
		// Each extra pass takes four stages: resize, pack guides, UI and restore.
		// Pass 2 keeps 7 to 10; Passes 3 to 5 follow the base stages, and their
		// descriptor heaps are created on first use.
		static constexpr unsigned kBaseStages = 16;
		static constexpr unsigned kExtraPassSteps = 4;
		static constexpr unsigned kExtraStagesEnd = kBaseStages + kExtraPassSteps * (static_cast<unsigned>(NeuralRendering::kExtraPasses) - 1);
		// Model rate: capture guides, compose motion, store the change, reproject it,
		// and on a second skip in a row compose the motion back and capture it again.
		static constexpr unsigned kModelRateStages = 6;
		static constexpr unsigned kStages = kExtraStagesEnd + kModelRateStages;
		static constexpr unsigned ModelRateStage(unsigned step) { return kExtraStagesEnd + step; }
		// pass is 1 for Pass 2 up to 4 for Pass 5.
		static constexpr unsigned ExtraPassStage(unsigned pass, unsigned step)
		{
			return pass <= 1 ? 7 + step : kBaseStages + kExtraPassSteps * (pass - 2) + step;
		}
		HRESULT Initialize(ID3D12Device* device);
		// The measured white point bound at t3 for this frame's dispatches, or null.
		void SetExposure(ID3D12Resource* exposure) { exposure_ = exposure; }
		// Broad and Detail's blurred tone bound at t4 for the next dispatches, or null.
		void SetTone(ID3D12Resource* tone) { tone_ = tone; }
		HRESULT Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot, unsigned stage,
			ResolveKernel kernel, const ResolveConstants& constants, ID3D12Resource* a, ID3D12Resource* b,
			ID3D12Resource* original, ID3D12Resource* output, ID3D12Resource* secondOutput = nullptr,
			ID3D12Resource* fourth = nullptr);
		// Test observation: descriptor tables written since initialization.
		std::uint64_t DescriptorWrites() const { return descriptorWrites_; }
	private:
		static HRESULT CreateHeap(ID3D12Device* device, Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& heap);
		struct DescriptorTable
		{
			// Retain identities so a released resource's address cannot be reused
			// and mistaken for an unchanged table. Rewrite only a retired slot.
			std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 7> resources;
			UINT width{}, height{};
			bool valid{};
		};
		std::array<std::array<DescriptorTable, kStages>, kCommandSlots> tables_;
		std::uint64_t descriptorWrites_{};
		Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
		std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 17> pipelines_;
		ID3D12Resource* exposure_{}; // Owned by the pass.
		ID3D12Resource* tone_{}; // Owned by the pass.
		std::array<std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, kStages>, kCommandSlots> heaps_;
	};
}
