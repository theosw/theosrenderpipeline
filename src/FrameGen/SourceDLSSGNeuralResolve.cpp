#include "SourceDLSSGNeuralResolve.h"
#include "TRPNeuralShaders.generated.h"
#include <algorithm>
#include <climits>

namespace TheosRenderPipeline::SourceDLSSG
{
	using Microsoft::WRL::ComPtr;
	HRESULT NeuralResolveKernels::Initialize(ID3D12Device* device)
	{
		if (!device) { return E_INVALIDARG; }
		if (root_) { return S_OK; }
		D3D12_DESCRIPTOR_RANGE ranges[2]{
			{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 },
			{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 3 }
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
		static_assert(CompiledNeuralShaders::Resolve.size() == std::tuple_size_v<decltype(pipelines_)>);
		for (unsigned i = 0; i < pipelines_.size(); ++i) {
			const auto& shader = CompiledNeuralShaders::Resolve[i];
			D3D12_SHADER_BYTECODE bytecode{ shader.data, shader.size };
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root.Get(); pso.CS = bytecode;
			hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipelines_[i]));
			if (FAILED(hr)) { return hr; }
		}
		for (auto& slot : heaps_) {
			for (auto& heap : slot) {
				D3D12_DESCRIPTOR_HEAP_DESC h{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
				hr = device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap));
				if (FAILED(hr)) { return hr; }
			}
		}
		root_ = std::move(root);
		return S_OK;
	}
	HRESULT NeuralResolveKernels::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
		unsigned stage, ResolveKernel kernel, const ResolveConstants& constants, ID3D12Resource* a,
		ID3D12Resource* b, ID3D12Resource* original, ID3D12Resource* output, ID3D12Resource* secondOutput)
	{
		if (!root_ || !device || !list || slot >= heaps_.size() || stage >= kStages || unsigned(kernel) >= pipelines_.size() ||
			!a || !output || a == output || b == output || original == output ||
			(secondOutput && (secondOutput == a || secondOutput == b || secondOutput == original || secondOutput == output)) ||
			(kernel == ResolveKernel::PackGuides && (!b || !secondOutput))) { return E_INVALIDARG; }
		const std::array<ID3D12Resource*, 5> bound{ a, b, original, output, secondOutput };
		auto& table = tables_[slot][stage];
		bool unchanged = table.valid;
		for (std::size_t i = 0; unchanged && i < bound.size(); ++i) { unchanged = table.resources[i].Get() == bound[i]; }
		auto* heap = heaps_[slot][stage].Get();
		if (!unchanged) {
			// Resource descriptions are immutable, so a retained identity needs no revalidation.
			std::array<D3D12_RESOURCE_DESC, 5> descs{};
			for (std::size_t i = 0; i < bound.size(); ++i) {
				if (!bound[i]) { continue; }
				const auto& d = descs[i] = bound[i]->GetDesc();
				if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
					d.SampleDesc.Count != 1 || !d.Width || !d.Height || d.Width > UINT_MAX) { return E_INVALIDARG; }
			}
			// Interop::Begin retired this slot, so its descriptors are no longer referenced.
			auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
			const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			for (std::size_t i = 0; i < 3; ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = bound[i] ? descs[i].Format : DXGI_FORMAT_R32G32B32A32_FLOAT;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				device->CreateShaderResourceView(bound[i], &srv, cpu); cpu.ptr += stride;
			}
			for (std::size_t i = 3; i < bound.size(); ++i) {
				D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = bound[i] ? descs[i].Format : DXGI_FORMAT_R32G32_FLOAT;
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				device->CreateUnorderedAccessView(bound[i], nullptr, &uav, cpu); cpu.ptr += stride;
			}
			for (std::size_t i = 0; i < bound.size(); ++i) { table.resources[i] = bound[i]; }
			table.width = static_cast<UINT>(descs[3].Width); table.height = descs[3].Height;
			table.valid = true;
			++descriptorWrites_;
		}
		// One barrier batch before and after the dispatch. Inputs may repeat.
		std::array<D3D12_RESOURCE_BARRIER, 5> barriers{};
		UINT count = 0;
		auto record = [&](bool entering) {
			count = 0;
			const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			auto add = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
				auto& barrier = barriers[count++]; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
					entering ? D3D12_RESOURCE_STATE_COMMON : state, entering ? state : D3D12_RESOURCE_STATE_COMMON };
			};
			for (std::size_t i = 0; i < 3; ++i) {
				if (bound[i] && std::find(bound.begin(), bound.begin() + i, bound[i]) == bound.begin() + i) { add(bound[i], read); }
			}
			add(output, write);
			if (secondOutput) { add(secondOutput, write); }
			list->ResourceBarrier(count, barriers.data());
		};
		record(true);
		list->SetDescriptorHeaps(1, &heap); list->SetComputeRootSignature(root_.Get());
		list->SetPipelineState(pipelines_[unsigned(kernel)].Get());
		list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
		list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
		list->Dispatch((table.width + 7) / 8, (table.height + 7) / 8, 1);
		record(false);
		return S_OK;
	}
}
