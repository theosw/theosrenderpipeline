#include "SourceDLSSGNeuralResolve.h"
#include "SourceDLSSGNeuralResolveShader.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <climits>
#include <cstdio>

namespace TheosRenderPipeline::SourceDLSSG
{
	using Microsoft::WRL::ComPtr;
	HRESULT NeuralResolveKernels::Initialize(ID3D12Device* device)
	{
		if (!device) { return E_INVALIDARG; }
		if (root_) { return S_OK; }
		D3D12_DESCRIPTOR_RANGE ranges[2]{
			{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 },
			{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3 }
		};
		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants = { 0, 0, sizeof(ResolveConstants) / 4 };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable = { 2, ranges };
		D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters = 2; desc.pParameters = params;
		ComPtr<ID3DBlob> signature, errors;
		auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
		if (FAILED(hr)) { return hr; }
		ComPtr<ID3D12RootSignature> root;
		hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root));
		if (FAILED(hr)) { return hr; }
		const char* entries[]{ "Downsample", "Residual", "Ratio" };
		for (unsigned i = 0; i < pipelines_.size(); ++i) {
			ComPtr<ID3DBlob> shader;
			hr = D3DCompile(kNeuralResolveShader, sizeof(kNeuralResolveShader) - 1, "TheosRenderPipeline-NR-resolve", nullptr, nullptr,
				entries[i], "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
			if (FAILED(hr)) {
				if (errors) { std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer())); }
				return hr;
			}
			D3D12_SHADER_BYTECODE bytecode{ shader->GetBufferPointer(), shader->GetBufferSize() };
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root.Get(); pso.CS = bytecode;
			hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipelines_[i]));
			if (FAILED(hr)) { return hr; }
		}
		for (auto& slot : heaps_) {
			for (auto& heap : slot) {
				D3D12_DESCRIPTOR_HEAP_DESC h{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
				hr = device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap));
				if (FAILED(hr)) { return hr; }
			}
		}
		root_ = std::move(root);
		return S_OK;
	}
	HRESULT NeuralResolveKernels::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
		unsigned stage, ResolveKernel kernel, const ResolveConstants& constants, ID3D12Resource* a,
		ID3D12Resource* b, ID3D12Resource* original, ID3D12Resource* output)
	{
		if (!root_ || !device || !list || slot >= heaps_.size() || stage >= kStages || unsigned(kernel) >= pipelines_.size() ||
			!a || !output || a == output || b == output || original == output) { return E_INVALIDARG; }
		auto& table = descriptors_[slot][stage];
		const std::array<ID3D12Resource*, 4> resources{ a, b, original, output };
		bool unchanged = table.valid;
		for (std::size_t i = 0; i < resources.size(); ++i) {
			unchanged = unchanged && table.resources[i].Get() == resources[i];
		}
		auto* heap = heaps_[slot][stage].Get();
		if (!unchanged) {
			std::array<D3D12_RESOURCE_DESC, 4> descriptions{};
			for (std::size_t i = 0; i < resources.size(); ++i) {
				if (!resources[i]) { continue; }
				const auto d = descriptions[i] = resources[i]->GetDesc();
				if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
					d.SampleDesc.Count != 1 || !d.Width || !d.Height || d.Width > UINT_MAX) { return E_INVALIDARG; }
			}
			// The caller has retired this exact slot before updating descriptors.
			auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
			const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			for (std::size_t i = 0; i < 3; ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = resources[i] ? descriptions[i].Format : DXGI_FORMAT_R32G32B32A32_FLOAT;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				device->CreateShaderResourceView(resources[i], &srv, cpu); cpu.ptr += stride;
			}
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = descriptions[3].Format;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
			for (std::size_t i = 0; i < resources.size(); ++i) { table.resources[i] = resources[i]; }
			table.width = static_cast<UINT>(descriptions[3].Width); table.height = descriptions[3].Height;
			table.valid = true;
		}
		auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
			list->ResourceBarrier(1, &barrier);
		};
		std::array<ID3D12Resource*, 3> inputs{ a, b, original };
		for (unsigned i = 0; i < inputs.size(); ++i) {
			if (inputs[i] && std::find(inputs.begin(), inputs.begin() + i, inputs[i]) == inputs.begin() + i) {
				transition(inputs[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			}
		}
		transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		list->SetDescriptorHeaps(1, &heap); list->SetComputeRootSignature(root_.Get());
		list->SetPipelineState(pipelines_[unsigned(kernel)].Get());
		list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
		list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
		list->Dispatch((table.width + 7) / 8, (table.height + 7) / 8, 1);
		transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		for (unsigned i = 0; i < inputs.size(); ++i) {
			if (inputs[i] && std::find(inputs.begin(), inputs.begin() + i, inputs[i]) == inputs.begin() + i) {
				transition(inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			}
		}
		return S_OK;
	}
}
