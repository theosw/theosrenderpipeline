#include "SourceDLSSGHDR.h"
#include "HDRColorimetry.h"
#include <d3dcompiler.h>
#include <cstring>

namespace TheosRenderPipeline::SourceDLSSG
{
	using Microsoft::WRL::ComPtr;
	namespace
	{
		// Linear BT.709 -> linear BT.2020 -> BT.2100 PQ inverse EOTF.
		// Primary matrices are derived in HDRColorimetry.h; brightness is an
		// explicit application calibration. This pass performs no tone mapping.
		constexpr const char* shader = R"(
cbuffer Colorimetry : register(b0) {
    float4 RedRowAndWhiteNits;
    float4 GreenRowAndPeakNits;
    float4 BlueRow;
};
Texture2D<float4> source : register(t0);
SamplerState sourceSampler : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float3 EncodePQ(float3 nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float3 luminance = saturate(nits / GreenRowAndPeakNits.w);
    float3 power = pow(luminance, m1);
    return pow((c1 + c2 * power) / (1.0 + c3 * power), m2);
}
float4 PS(Vertex v) : SV_Target {
    float3 linear709 = source.Sample(sourceSampler, v.uv).rgb;
    float3 linear2020 = float3(dot(RedRowAndWhiteNits.xyz, linear709),
        dot(GreenRowAndPeakNits.xyz, linear709), dot(BlueRow.xyz, linear709));
    // Clip after gamut conversion: negative 709 components can represent
    // valid colours in the wider 2020 gamut. The output is full-range HDR10.
    return float4(EncodePQ(linear2020 * RedRowAndWhiteNits.w), 1.0);
})";
		void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
			list->ResourceBarrier(1, &b);
		}
	}

	HRESULT HDRPass::Initialize(ID3D12Device* device)
	{
		if (pipeline_) { return S_OK; }
		D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
		D3D12_ROOT_PARAMETER parameters[2]{};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[0].DescriptorTable = { 1, &range }; parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[1].Constants = { 0, 0, sizeof(HDRColorimetry::ShaderParameters) / sizeof(float) };
		parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC desc{ 2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
		ComPtr<ID3DBlob> serialized, error, vs, ps;
		auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
		if (FAILED(hr)) { return hr; }
		hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_));
		if (FAILED(hr)) { return hr; }
		hr = D3DCompile(shader, std::strlen(shader), "SourceDLSSGHDR", nullptr, nullptr, "VS", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error);
		if (FAILED(hr)) { return hr; }
		hr = D3DCompile(shader, std::strlen(shader), "SourceDLSSGHDR", nullptr, nullptr, "PS", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error);
		if (FAILED(hr)) { return hr; }
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = root_.Get(); pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() }; pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		auto& blend = pso.BlendState.RenderTarget[0];
		blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
		blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
		blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pso.SampleMask = UINT_MAX; pso.SampleDesc.Count = 1;
		pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.RasterizerState.DepthClipEnable = TRUE;
		pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
		// Publish pipeline last so partial initialization cannot look ready.
		for (std::size_t slot = 0; slot < kCommandSlots; ++slot) {
			D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(srv_[slot].ReleaseAndGetAddressOf())); if (FAILED(hr)) { return hr; }
			heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv_[slot].ReleaseAndGetAddressOf())); if (FAILED(hr)) { return hr; }
		}
		return device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_));
	}

	HRESULT HDRPass::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot, ID3D12Resource* source, ID3D12Resource* destination)
	{
		if (!device || !list || !source || !destination || source == destination || slot >= kCommandSlots) { return E_INVALIDARG; }
		const auto src = source->GetDesc(), dst = destination->GetDesc();
		if (src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dst.Dimension != src.Dimension ||
			src.Width != dst.Width || src.Height != dst.Height || src.DepthOrArraySize != 1 || dst.DepthOrArraySize != 1 ||
			src.MipLevels != 1 || dst.MipLevels != 1 || src.SampleDesc.Count != 1 || dst.SampleDesc.Count != 1 ||
			src.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || dst.Format != DXGI_FORMAT_R10G10B10A2_UNORM ||
			(src.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || !(dst.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) { return E_INVALIDARG; }
		const auto initialized = Initialize(device); if (FAILED(initialized)) { return initialized; }
		// The owner has retired this command-ring slot before updating its views.
		source_[slot] = source; destination_[slot] = destination;
		D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = src.Format;
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(source, &view, srv_[slot]->GetCPUDescriptorHandleForHeapStart());
		const auto rtv = rtv_[slot]->GetCPUDescriptorHandleForHeapStart();
		device->CreateRenderTargetView(destination, nullptr, rtv);
		Transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
		auto* heap = srv_[slot].Get(); list->SetDescriptorHeaps(1, &heap);
		list->SetGraphicsRootSignature(root_.Get()); list->SetPipelineState(pipeline_.Get());
		list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
		list->SetGraphicsRoot32BitConstants(1, sizeof(HDRColorimetry::ShaderParameters) / sizeof(float), &HDRColorimetry::kShaderParameters, 0);
		const D3D12_VIEWPORT viewport{ 0, 0, static_cast<float>(dst.Width), static_cast<float>(dst.Height), 0, 1 };
		const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(dst.Width), static_cast<LONG>(dst.Height) };
		list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
		list->OMSetRenderTargets(1, &rtv, FALSE, nullptr); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->DrawInstanced(3, 1, 0, 0);
		Transition(list, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		Transition(list, destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
		return S_OK;
	}
}
