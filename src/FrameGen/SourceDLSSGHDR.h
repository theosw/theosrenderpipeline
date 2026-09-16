#pragma once
#include "SourceDLSSGInterop.h"
#include <dxgi1_4.h>

namespace TheosRenderPipeline::SourceDLSSG
{
	constexpr DXGI_FORMAT PresentationFormat(DXGI_FORMAT gameFormat)
	{
		return gameFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_FORMAT_R10G10B10A2_UNORM : gameFormat;
	}
	constexpr DXGI_COLOR_SPACE_TYPE PresentationColorSpace(DXGI_FORMAT gameFormat, DXGI_COLOR_SPACE_TYPE color)
	{
		return gameFormat == DXGI_FORMAT_R16G16B16A16_FLOAT && color == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ?
			DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : color;
	}
	// Converts calibrated linear BT.709 FP16 to full-range BT.2020 PQ HDR10.
	// The caller must retire a command slot before reuse and Drain before release.
	class HDRPass final
	{
	public:
		HRESULT Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
			ID3D12Resource* source, ID3D12Resource* destination);
	private:
		HRESULT Initialize(ID3D12Device* device);
		Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
		std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, kCommandSlots> srv_, rtv_;
		std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kCommandSlots> source_, destination_;
	};
}
