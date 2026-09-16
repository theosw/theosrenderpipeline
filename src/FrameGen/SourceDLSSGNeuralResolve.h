#pragma once
#include "SourceDLSSGInterop.h"
#include <array>
#include <span>

namespace TheosRenderPipeline::SourceDLSSG
{
	enum class ResolveKernel : unsigned { Downsample, Residual, Ratio };
	struct ResolveConstants
	{
		UINT sourceWidth{}, sourceHeight{}, targetWidth{}, targetHeight{};
		UINT sourceIsBGRA{}, mode{}, passthrough{ 1 }, pad{};
		UINT workWidth{}, workHeight{};
		float transferStrength{ 1 }, colourStrength{ 1 }, maxRatio{ 2 }, whitePoint{ 1 };
	};
	static_assert(sizeof(ResolveConstants) == 56);
	// All resources enter and leave COMMON. Slot retirement is the caller's
	// responsibility. Each dispatch in a frame needs a distinct stage index.
	class NeuralResolveKernels
	{
	public:
		static constexpr unsigned kStages = 4;
		HRESULT Initialize(ID3D12Device* device);
		HRESULT Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot, unsigned stage,
			ResolveKernel kernel, const ResolveConstants& constants, ID3D12Resource* a, ID3D12Resource* b,
			ID3D12Resource* original, ID3D12Resource* output);
	private:
		Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
		std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 3> pipelines_;
		std::array<std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, kStages>, kCommandSlots> heaps_;
	};
}
