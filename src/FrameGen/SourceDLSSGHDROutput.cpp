#include "SourceDLSSGHDROutput.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cwchar>
#include <vector>

namespace TheosRenderPipeline::SourceDLSSG
{
	using Microsoft::WRL::ComPtr;
	namespace
	{
		// Mirrors the CPU reference in HDROutput.h. Keep both in step.
		constexpr const char* shader = R"(
cbuffer Output : register(b0) {
    float4 Red; float4 Green; float4 Blue;
    float4 Scale; // paper nits, UI nits, expansion start, maximum scale
    float4 Mode;  // transfer (0 = 2.2, 1 = sRGB), passthrough, expand whole frame
};
Texture2D<float4> composite : register(t0);
Texture2D<float4> ui : register(t1);
Texture2D<float4> scene : register(t2);
struct Vertex { float4 position : SV_Position; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v;
    float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float3 Decode(float3 c) {
    c = saturate(c);
    if (Mode.x > 0.5) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
    return pow(c, 2.2);
}
float3 Expand(float3 l) {
    float y = dot(l, float3(0.2126, 0.7152, 0.0722));
    float v = max(l.r, max(l.g, l.b));
    if (Scale.w <= 1.0 || y <= 0.0 || v <= 0.0) { return l; }
    float s = pow(Scale.w, smoothstep(Scale.z, 1.0, y));
    s = max(min(s, Scale.w / v), 1.0);
    return l * s;
}
float3 EncodeHDR10(float3 nits709) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 n = max(float3(dot(Red.xyz, nits709), dot(Green.xyz, nits709), dot(Blue.xyz, nits709)), 0.0);
    float3 p = pow(saturate(n / 10000.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float3 EncodeScene(float3 c) { return EncodeHDR10(Expand(Decode(c)) * Scale.x); }
float3 EncodeUI(float3 c) { return EncodeHDR10(Decode(c) * Scale.y); }
// Premultiplied UI: the part covered by alpha is encoded at UI brightness and
// premultiplied again; light beyond alpha (additive glows) is added separately.
float3 EncodeUIPremultiplied(float3 rgb, float a) {
    float3 covered = min(rgb, a);
    float3 straight = a > (1.0 / 1024.0) ? covered / a : 0.0;
    return saturate(EncodeUI(straight) * a + EncodeUI(max(rgb - covered, 0.0)));
}

float4 PSEncode(Vertex v) : SV_Target {
    float4 c = composite.Load(int3(v.position.xy, 0));
    if (Mode.y > 0.5) { return float4(c.rgb, 1.0); }
    // World frames without a separate UI layer still expand; menus stay at UI brightness.
    return float4(Mode.z > 0.5 ? EncodeScene(c.rgb) : EncodeUI(c.rgb), 1.0);
}

struct Targets { float4 backbuffer : SV_Target0; float4 hudless : SV_Target1; float4 ui : SV_Target2; };
Targets PSCompose(Vertex v) {
    int3 p = int3(v.position.xy, 0);
    float4 c = composite.Load(p), u = ui.Load(p), s = scene.Load(p);
    float a = saturate(u.a);
    // The native UI layer is premultiplied in the producer's SDR encoding.
    float3 uiPQ = EncodeUIPremultiplied(u.rgb, a);
    // Where the composed frame is the tagged layers, use the exact HUD-less
    // scene so the DLSS-G identity holds; recovering it from 8-bit codes would
    // amplify rounding in expanded highlights. Where other content was drawn
    // (a late overlay), recover the scene behind the UI from the composed frame.
    // The blend between them is gradual, so small differences do not form edges.
    float3 difference = abs(c.rgb - saturate(u.rgb + (1.0 - a) * s.rgb));
    float extra = saturate((max(difference.r, max(difference.g, difference.b)) - 1.5 / 255.0) / (2.0 / 255.0));
    float3 recovered = a < (1.0 - 1.0 / 512.0) ? saturate((c.rgb - u.rgb) / (1.0 - a)) : s.rgb;
    float3 behind = lerp(s.rgb, recovered, extra);
    Targets t;
    t.backbuffer = float4(saturate(uiPQ + (1.0 - a) * EncodeScene(behind)), 1.0);
    t.hudless = float4(EncodeScene(s.rgb), 1.0);
    t.ui = float4(uiPQ, a);
    return t;
})";

		void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
			list->ResourceBarrier(1, &b);
		}

		bool Plain2D(const D3D12_RESOURCE_DESC& d)
		{
			return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
				d.MipLevels == 1 && d.SampleDesc.Count == 1;
		}

		bool Readable(ID3D12Resource* r, UINT width, UINT height)
		{
			if (!r) { return false; }
			const auto d = r->GetDesc();
			return Plain2D(d) && d.Width == width && d.Height == height && !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
		}

		bool Writable(ID3D12Resource* r, UINT width, UINT height, DXGI_FORMAT format)
		{
			if (!r) { return false; }
			const auto d = r->GetDesc();
			return Plain2D(d) && d.Width == width && d.Height == height && d.Format == format &&
				(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
		}
	}

	DisplayHDR QueryDisplayHDR(IDXGIFactory1* factory, HWND window)
	{
		DisplayHDR result;
		if (!factory || !window) { return result; }
		result.monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
			ComPtr<IDXGIOutput> output;
			for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
				ComPtr<IDXGIOutput6> output6;
				DXGI_OUTPUT_DESC1 desc{};
				if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc)) || desc.Monitor != result.monitor) { continue; }
				result.known = true;
				result.active = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
				result.maxLuminance = desc.MaxLuminance;
				std::copy(std::begin(desc.DeviceName), std::end(desc.DeviceName), result.deviceName.begin());
				result.deviceName.back() = L'\0';
				return result;
			}
		}
		return result;
	}

	float QuerySDRWhiteNits(const wchar_t* gdiDeviceName)
	{
		if (!gdiDeviceName || !*gdiDeviceName) { return 0.0f; }
		UINT32 pathCount{}, modeCount{};
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) { return 0.0f; }
		std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) { return 0.0f; }
		for (UINT32 i = 0; i < pathCount; ++i) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
			source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; source.header.size = sizeof(source);
			source.header.adapterId = paths[i].sourceInfo.adapterId; source.header.id = paths[i].sourceInfo.id;
			if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || std::wcscmp(source.viewGdiDeviceName, gdiDeviceName) != 0) { continue; }
			DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
			white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL; white.header.size = sizeof(white);
			white.header.adapterId = paths[i].targetInfo.adapterId; white.header.id = paths[i].targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) { return 0.0f; }
			// Fixed point: 1000 = the 80-nit scRGB reference white.
			return static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;
		}
		return 0.0f;
	}

	HRESULT HDROutputPass::Initialize(ID3D12Device* device)
	{
		if (compose_ && encode_) { return S_OK; }
		D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
		D3D12_ROOT_PARAMETER parameters[2]{};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[0].DescriptorTable = { 1, &range }; parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[1].Constants = { 0, 0, sizeof(HDROutput::ShaderConstants) / sizeof(float) };
		parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC desc{ 2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		ComPtr<ID3DBlob> serialized, error, vs, encodePS, composePS;
		auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
		if (FAILED(hr)) { return hr; }
		hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_));
		if (FAILED(hr)) { return hr; }
		const auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
			return D3DCompile(shader, std::strlen(shader), "SourceDLSSGHDROutput", nullptr, nullptr, entry, target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &error);
		};
		if (FAILED(hr = compile("VS", "vs_5_1", vs)) || FAILED(hr = compile("PSEncode", "ps_5_1", encodePS)) ||
			FAILED(hr = compile("PSCompose", "ps_5_1", composePS))) { return hr; }
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = root_.Get(); pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		for (auto& blend : pso.BlendState.RenderTarget) {
			blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
			blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
			blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		}
		pso.SampleMask = UINT_MAX; pso.SampleDesc.Count = 1;
		pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.RasterizerState.DepthClipEnable = TRUE;
		pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		for (std::size_t slot = 0; slot < kCommandSlots; ++slot) {
			D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(srv_[slot].ReleaseAndGetAddressOf())); if (FAILED(hr)) { return hr; }
			heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv_[slot].ReleaseAndGetAddressOf())); if (FAILED(hr)) { return hr; }
		}
		ComPtr<ID3D12PipelineState> encode, compose;
		pso.PS = { encodePS->GetBufferPointer(), encodePS->GetBufferSize() };
		pso.NumRenderTargets = 1; pso.RTVFormats[0] = kOutputFormat;
		hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&encode)); if (FAILED(hr)) { return hr; }
		pso.PS = { composePS->GetBufferPointer(), composePS->GetBufferSize() };
		pso.NumRenderTargets = 3; pso.RTVFormats[1] = kOutputFormat; pso.RTVFormats[2] = kUIFormat;
		hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&compose)); if (FAILED(hr)) { return hr; }
		// Publish pipelines last so partial initialization cannot look ready.
		encode_ = encode; compose_ = compose;
		return S_OK;
	}

	HRESULT HDROutputPass::EnableTiming(ID3D12Device* device, std::uint64_t frequency)
	{
		if (timestamps_) { return S_OK; }
		if (!device || !frequency) { return E_INVALIDARG; }
		D3D12_QUERY_HEAP_DESC heapDesc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kCommandSlots, 0 };
		ComPtr<ID3D12QueryHeap> heap;
		auto hr = device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap));
		if (FAILED(hr)) { return hr; }
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 2 * kCommandSlots * sizeof(std::uint64_t);
		desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_READBACK;
		ComPtr<ID3D12Resource> readback;
		hr = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr, IID_PPV_ARGS(&readback));
		if (FAILED(hr)) { return hr; }
		void* mapped{};
		// Persistently mapped; each pair is read only after its slot has retired.
		hr = readback->Map(0, nullptr, &mapped);
		if (FAILED(hr)) { return hr; }
		timestamps_ = heap; timestampReadback_ = readback;
		mappedTimestamps_ = static_cast<const std::uint64_t*>(mapped);
		timestampFrequency_ = frequency;
		return S_OK;
	}

	void HDROutputPass::HarvestTiming(std::size_t slot)
	{
		if (!mappedTimestamps_ || !timingPending_[slot]) { return; }
		timingPending_[slot] = false;
		const auto begin = mappedTimestamps_[2 * slot], end = mappedTimestamps_[2 * slot + 1];
		if (end <= begin) { return; }
		const double us = static_cast<double>(end - begin) * 1e6 / static_cast<double>(timestampFrequency_);
		++harvested_.samples; harvested_.totalUs += us; harvested_.maxUs = (std::max)(harvested_.maxUs, us);
	}

	bool HDROutputPass::TargetsMatch(UINT width, UINT height) const
	{
		return hudless_ && ui_ && hudless_->GetDesc().Width == width && hudless_->GetDesc().Height == height;
	}

	HRESULT HDROutputPass::CreateTargets(ID3D12Device* device, UINT width, UINT height)
	{
		if (!device || !width || !height) { return E_INVALIDARG; }
		hudless_.Reset(); ui_.Reset();
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
		desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		ComPtr<ID3D12Resource> hudless, ui;
		desc.Format = kOutputFormat;
		auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&hudless));
		if (FAILED(hr)) { return hr; }
		desc.Format = kUIFormat;
		hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ui));
		if (FAILED(hr)) { return hr; }
		hudless_ = hudless; ui_ = ui;
		return S_OK;
	}

	HRESULT HDROutputPass::RecordCompose(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
		const HDROutput::ShaderConstants& constants, ID3D12Resource* composite, ID3D12Resource* ui,
		ID3D12Resource* scene, ID3D12Resource* backbuffer)
	{
		return Record(device, list, slot, constants, composite, ui, scene, backbuffer, true);
	}

	HRESULT HDROutputPass::RecordEncode(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
		const HDROutput::ShaderConstants& constants, ID3D12Resource* composite, ID3D12Resource* backbuffer)
	{
		return Record(device, list, slot, constants, composite, nullptr, nullptr, backbuffer, false);
	}

	HRESULT HDROutputPass::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
		const HDROutput::ShaderConstants& constants, ID3D12Resource* composite, ID3D12Resource* ui,
		ID3D12Resource* scene, ID3D12Resource* backbuffer, bool compose)
	{
		if (!device || !list || !composite || !backbuffer || slot >= kCommandSlots) { return E_INVALIDARG; }
		const auto out = backbuffer->GetDesc();
		const UINT width = static_cast<UINT>(out.Width), height = out.Height;
		if (!Writable(backbuffer, width, height, kOutputFormat) || !Readable(composite, width, height) || composite == backbuffer) { return E_INVALIDARG; }
		if (compose && (!Readable(ui, width, height) || !Readable(scene, width, height) || !TargetsMatch(width, height) ||
			ui == scene || ui == composite || scene == composite)) { return E_INVALIDARG; }
		const auto initialized = Initialize(device); if (FAILED(initialized)) { return initialized; }
		// The owner has retired this command-ring slot before updating its views,
		// so its previous timestamp pair is complete.
		HarvestTiming(slot);
		retained_[slot] = { composite, ui, scene, backbuffer };
		const auto first = static_cast<UINT>(2 * slot);
		if (timestamps_) { list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first); }
		std::array<ID3D12Resource*, 3> inputs{ composite, compose ? ui : composite, compose ? scene : composite };
		const auto srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto srv = srv_[slot]->GetCPUDescriptorHandleForHeapStart();
		for (auto* input : inputs) {
			D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = input->GetDesc().Format;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(input, &view, srv);
			srv.ptr += srvSize;
		}
		std::array<ID3D12Resource*, 3> outputs{ backbuffer, hudless_.Get(), ui_.Get() };
		const UINT targets = compose ? 3 : 1;
		const auto rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> rtvs{};
		for (UINT i = 0; i < targets; ++i) {
			rtvs[i] = rtv_[slot]->GetCPUDescriptorHandleForHeapStart();
			rtvs[i].ptr += i * rtvSize;
			device->CreateRenderTargetView(outputs[i], nullptr, rtvs[i]);
		}
		Transition(list, composite, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (compose) {
			Transition(list, ui, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			Transition(list, scene, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		for (UINT i = 0; i < targets; ++i) { Transition(list, outputs[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET); }
		auto* heap = srv_[slot].Get(); list->SetDescriptorHeaps(1, &heap);
		list->SetGraphicsRootSignature(root_.Get()); list->SetPipelineState(compose ? compose_.Get() : encode_.Get());
		list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
		list->SetGraphicsRoot32BitConstants(1, sizeof(HDROutput::ShaderConstants) / sizeof(float), &constants, 0);
		const D3D12_VIEWPORT viewport{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
		const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
		list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
		list->OMSetRenderTargets(targets, rtvs.data(), FALSE, nullptr); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->DrawInstanced(3, 1, 0, 0);
		for (UINT i = 0; i < targets; ++i) { Transition(list, outputs[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON); }
		if (compose) {
			Transition(list, scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, ui, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		}
		Transition(list, composite, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		if (timestamps_) {
			list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
			list->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2,
				timestampReadback_.Get(), first * sizeof(std::uint64_t));
			timingPending_[slot] = true;
		}
		return S_OK;
	}
}
