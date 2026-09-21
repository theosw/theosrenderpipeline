#include <PCH.h>
#include "SourceDLSSGNeuralRendering.h"
#include <chrono>
#include <d3dcompiler.h>

namespace TheosRenderPipeline::SourceDLSSG
{
	namespace
	{
		void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
			D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			if (!resource) { return; } // The early world-only stage has no UI/composed inputs.
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
			list->ResourceBarrier(1, &b);
		}
		bool Texture(ID3D12Resource* resource)
		{
			if (!resource) { return false; }
			const auto d = resource->GetDesc();
			return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width && d.Height &&
				d.Width <= UINT_MAX && d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1;
		}
		bool SameSize(ID3D12Resource* a, ID3D12Resource* b)
		{
			return Texture(a) && Texture(b) && a->GetDesc().Width == b->GetDesc().Width &&
				a->GetDesc().Height == b->GetDesc().Height;
		}
		constexpr char kCompose[] = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> ui : register(t1);
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
    uint w,h; output.GetDimensions(w,h); if (p.x >= w || p.y >= h) return;
    float4 s = scene.Load(int3(p.xy,0)), u = ui.Load(int3(p.xy,0));
    output[p.xy] = float4(s.rgb * (1.0 - u.a) + u.rgb, max(s.a, u.a));
})";
	}

	NeuralPass::~NeuralPass()
	{
		if (timestampReadback_ && mappedTimestamps_) {
			D3D12_RANGE written{ 0, 0 };
			timestampReadback_->Unmap(0, &written);
			mappedTimestamps_ = nullptr;
		}
	}

	void NeuralPass::InitializeTelemetry(ID3D12Device* device, std::uint64_t timestampFrequency)
	{
		if (telemetryAttempted_ || !device || !timestampFrequency) { return; }
		telemetryAttempted_ = true;
		timestampFrequency_ = timestampFrequency;
		D3D12_QUERY_HEAP_DESC query{};
		query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		query.Count = static_cast<UINT>(kCommandSlots * 2);
		if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&timestampHeap_)))) {
			timestampFrequency_ = 0;
			logger::warn("[DLSSNR Source] GPU boundary timing unavailable: timestamp heap creation failed");
			return;
		}
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = sizeof(std::uint64_t) * kCommandSlots * 2;
		buffer.Height = 1;
		buffer.DepthOrArraySize = 1;
		buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestampReadback_)))) {
			timestampHeap_.Reset();
			timestampFrequency_ = 0;
			logger::warn("[DLSSNR Source] GPU boundary timing unavailable: readback allocation failed");
			return;
		}
		D3D12_RANGE read{ 0, static_cast<SIZE_T>(buffer.Width) };
		if (FAILED(timestampReadback_->Map(0, &read, reinterpret_cast<void**>(&mappedTimestamps_)))) {
			timestampReadback_.Reset();
			timestampHeap_.Reset();
			timestampFrequency_ = 0;
			logger::warn("[DLSSNR Source] GPU boundary timing unavailable: readback mapping failed");
		}
	}

	void NeuralPass::HarvestTelemetry(std::size_t slot)
	{
		if (slot >= pendingTiming_.size() || !pendingTiming_[slot].pending || !mappedTimestamps_) { return; }
		const auto& pending = pendingTiming_[slot];
		telemetry_.Record(pending.evaluation, mappedTimestamps_[slot * 2],
			mappedTimestamps_[slot * 2 + 1], timestampFrequency_, pending.cpuRecordNanoseconds);
		pendingTiming_[slot] = {};
	}

	void NeuralPass::RetireTelemetry()
	{
		for (std::size_t slot = 0; slot < pendingTiming_.size(); ++slot) {
			HarvestTelemetry(slot);
		}
	}

	bool NeuralPass::Initialize(ID3D12Device* device, const NeuralOptions& options,
		ID3D12Resource* motion, ID3D12Resource* hudless, ID3D12Resource* composed)
	{
		auto check = [&](HRESULT hr, const char* operation) {
			if (FAILED(hr)) { status_ = std::format("{} failed 0x{:08X}", operation, static_cast<UINT>(hr)); }
			return SUCCEEDED(hr);
		};
		const auto sceneDesc = hudless->GetDesc(), outputDesc = composed ? composed->GetDesc() : sceneDesc;
		const auto reconstruction = NeuralRendering::SanitizeReconstruction(options.reconstruction);
		const auto method = NeuralRendering::EffectiveResolve(reconstruction);
		const auto workWidth = NeuralRendering::ModelExtent(static_cast<UINT>(sceneDesc.Width), reconstruction);
		const auto workHeight = NeuralRendering::ModelExtent(sceneDesc.Height, reconstruction);
		if (NeedsRecreation(options, static_cast<UINT>(motion->GetDesc().Width), motion->GetDesc().Height)) {
			status_ = "NR settings or guide dimensions changed without retiring the previous pass"; return false;
		}
		if (corrected_ && (!SameSize(corrected_.Get(), hudless) || corrected_->GetDesc().Format != sceneDesc.Format ||
			(!options.WorldOnly() && (!SameSize(composed_.Get(), composed) || composed_->GetDesc().Format != outputDesc.Format)))) {
			status_ = "NR dimensions changed without retiring the previous pass"; return false;
		}
		if (!corrected_) {
			fusedColor_ = reconstruction.fusedPreparation && method == NeuralRendering::ResolveMethod::Ratio &&
				(reconstruction.producerColor || reconstruction.colorIsHDR) &&
				(reconstruction.inputScale < 1 || reconstruction.peripheralCompression) &&
				(sceneDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || sceneDesc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || sceneDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
			auto create = [&](D3D12_RESOURCE_DESC desc, Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
				// The private NR outputs never alias a D3D11 resource or the swapchain.
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
				return check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)), "NR output allocation");
			};
			if (!create(sceneDesc, corrected_) || (!options.WorldOnly() && !create(outputDesc, composed_))) { return false; }
			if (options.passes == 2) {
				auto secondDesc = sceneDesc; secondDesc.Width = workWidth; secondDesc.Height = workHeight;
				if (!create(secondDesc, secondOutput_)) { return false; }
			}
			if (method != NeuralRendering::ResolveMethod::Auto) {
				if (!check(resolve_.Initialize(device), "NR resolve kernels")) { return false; }
				auto workDesc = sceneDesc; workDesc.Width = workWidth; workDesc.Height = workHeight;
				if (!create(workDesc, workOutput_)) { return false; }
				if ((reconstruction.inputScale < 1 || reconstruction.peripheralCompression) && !create(workDesc, workColor_)) { return false; }
				if (reconstruction.peripheralCompression) {
					auto guideDesc = workDesc; guideDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
					if (!create(guideDesc, packedMotion_)) { return false; }
					guideDesc.Format = DXGI_FORMAT_R32_FLOAT;
					if (!create(guideDesc, packedDepth_)) { return false; }
					if (!options.WorldOnly() && !create(workDesc, packedUI_)) { return false; }
				}
				if (method == NeuralRendering::ResolveMethod::Residual) {
					auto residualDesc = workDesc; residualDesc.Width = sceneDesc.Width;
					residualDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
					if (!create(residualDesc, residual_)) { return false; }
				} else if (!fusedColor_ && (reconstruction.colorIsHDR || reconstruction.producerColor) && !create(sceneDesc, encoded_)) { return false; }
			}
			if (!options.WorldOnly()) {
				D3D12_DESCRIPTOR_RANGE ranges[2]{};
				ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
				ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2 };
				D3D12_ROOT_PARAMETER parameter{};
				parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				parameter.DescriptorTable = { 2, ranges };
				D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.NumParameters = 1; rootDesc.pParameters = &parameter;
				Microsoft::WRL::ComPtr<ID3DBlob> signature, errors, shader;
				if (!check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
					&signature, &errors), "NR compose root serialization") ||
					!check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
						IID_PPV_ARGS(&root_)), "NR compose root") ||
					!check(D3DCompile(kCompose, sizeof(kCompose) - 1, "SourceDLSSG-NR-compose", nullptr, nullptr,
						"main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors), "NR compose shader")) { return false; }
				D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root_.Get();
				pso.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
				if (!check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_)), "NR compose pipeline")) { return false; }
				for (auto& heap : heaps_) {
					D3D12_DESCRIPTOR_HEAP_DESC d{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
					if (!check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap)), "NR compose descriptors")) { return false; }
				}
			}
		}
		NeuralRendering::FeatureSession::CreateInfo info;
		info.device = device; info.runtimePath = options.runtimePath;
		info.allowReconstructionRuntime = true;
		info.bottleneckReuse = reconstruction.bottleneckReuse;
		info.displayWidth = workWidth; info.displayHeight = workHeight;
		info.renderWidth = reconstruction.peripheralCompression ? workWidth : static_cast<UINT>(motion->GetDesc().Width);
		info.renderHeight = reconstruction.peripheralCompression ? workHeight : motion->GetDesc().Height;
		// Each identified runtime contract selects its own default preset.
		info.networkPreset = reconstruction.preset == 0 ? -1 : reconstruction.preset;
		if (!feature_.EnsureInitialized(info)) { status_ = feature_.Status(); return false; }
		if (options.passes == 2 && !secondFeature_.EnsureInitialized(info)) {
			status_ = "NR second feature: " + secondFeature_.Status(); return false;
		}
		if (method != NeuralRendering::ResolveMethod::Auto && !NeuralRendering::UsesReconstructionContract(feature_.Build())) {
			status_ = "Legacy NR supports Auto reconstruction only"; return false;
		}
		reconstruction_ = reconstruction;
		runtimePath_ = options.runtimePath;
		// Recreation watches the producer's extent, not the private packed guides.
		guideWidth_ = static_cast<UINT>(motion->GetDesc().Width); guideHeight_ = motion->GetDesc().Height;
		beforeUpscaling_ = options.beforeUpscaling;
		worldOnly_ = options.WorldOnly();
		passes_ = options.passes;
		return true;
	}

	bool NeuralPass::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
		const NeuralOptions& options, bool reset, bool depthInverted, float scaleX, float scaleY,
		ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* ui,
		ID3D12Resource* hudless, ID3D12Resource* composed, std::uint64_t timestampFrequency)
	{
		if (!device || !list || slot >= heaps_.size() || options.passes < 1 || options.passes > 2 || !SameSize(motion, depth) ||
			!Texture(hudless) || (options.WorldOnly() ? (ui || composed || options.tuning.uiCorrection) :
			(!SameSize(hudless, composed) || !SameSize(ui, hudless) || hudless == composed)) ||
			!std::isfinite(scaleX) || !std::isfinite(scaleY) || !scaleX || !scaleY) {
			status_ = "NR source input contract is incomplete"; return false;
		}
		if (!Initialize(device, options, motion, hudless, composed)) { return false; }
		InitializeTelemetry(device, timestampFrequency);
		// Interop::Begin retired this exact command slot before Record was called.
		// Reading its previous query pair therefore never adds a CPU wait.
		HarvestTelemetry(slot);
		const auto reconstruction = NeuralRendering::SanitizeReconstruction(options.reconstruction);
		const auto method = NeuralRendering::EffectiveResolve(reconstruction);
		const bool resolving = method != NeuralRendering::ResolveMethod::Auto;
		auto* featureColor = hudless;
		auto* featureOutput = resolving ? workOutput_.Get() : corrected_.Get();
		ResolveConstants constants;
		constants.sourceWidth = static_cast<UINT>(hudless->GetDesc().Width); constants.sourceHeight = hudless->GetDesc().Height;
		constants.targetWidth = static_cast<UINT>(featureOutput->GetDesc().Width); constants.targetHeight = featureOutput->GetDesc().Height;
		constants.workWidth = constants.targetWidth; constants.workHeight = constants.targetHeight;
		constants.passthrough = !reconstruction.colorIsHDR;
		constants.producerColor = reconstruction.producerColor;
		constants.transferStrength = reconstruction.transferStrength; constants.colourStrength = reconstruction.colourStrength;
		constants.maxRatio = reconstruction.maxRatio; constants.whitePoint = reconstruction.whitePoint;
		constants.peripheral = reconstruction.peripheralCompression;
		constants.encodedFormat = hudless->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 :
			hudless->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM ? 2 : 0;
		constants.guideWidth = static_cast<UINT>(motion->GetDesc().Width); constants.guideHeight = motion->GetDesc().Height;
		constants.motionScaleX = scaleX; constants.motionScaleY = scaleY;
		auto* featureMotion = motion;
		auto* featureDepth = depth;
		auto* featureUI = ui;
		auto dispatch = [&](unsigned stage, ResolveKernel kernel, ID3D12Resource* a, ID3D12Resource* b,
			ID3D12Resource* original, ID3D12Resource* output, ID3D12Resource* secondOutput = nullptr) {
			const auto hr = resolve_.Record(device, list, slot, stage, kernel, constants, a, b, original, output, secondOutput);
			if (FAILED(hr)) { status_ = std::format("NR reconstruction recording failed 0x{:08X}", static_cast<UINT>(hr)); }
			return SUCCEEDED(hr);
		};
		if (resolving) {
			if (encoded_) {
				constants.targetWidth = constants.sourceWidth; constants.targetHeight = constants.sourceHeight;
				if (!dispatch(0, ResolveKernel::Ratio, hudless, nullptr, nullptr, encoded_.Get())) { return false; }
				featureColor = encoded_.Get();
			}
			if (workColor_) {
				constants.targetWidth = constants.workWidth; constants.targetHeight = constants.workHeight;
				if (!dispatch(1, fusedColor_ ? ResolveKernel::PrepareColor : ResolveKernel::Downsample, featureColor, nullptr, nullptr, workColor_.Get())) { return false; }
				featureColor = workColor_.Get();
			}
		}
		if (reconstruction.peripheralCompression) {
			if (reconstruction.fusedPreparation) {
				if (!dispatch(4, ResolveKernel::PackGuides, depth, motion, nullptr, packedDepth_.Get(), packedMotion_.Get())) { return false; }
			} else if (!dispatch(4, ResolveKernel::PackDepth, depth, nullptr, nullptr, packedDepth_.Get()) ||
				!dispatch(5, ResolveKernel::PackMotion, motion, nullptr, nullptr, packedMotion_.Get())) { return false; }
			featureDepth = packedDepth_.Get(); featureMotion = packedMotion_.Get();
			if (ui) {
				if (!dispatch(6, ResolveKernel::Downsample, ui, nullptr, nullptr, packedUI_.Get())) { return false; }
				featureUI = packedUI_.Get();
			}
		}
		// NR UI correction reads Backbuffer's existing pixels. The reconstruction
		// output/backbuffer alias must therefore contain the original scene, not
		// fresh allocation contents or last frame's NR result. UI remains separate
		// here because TheosRenderPipeline composes it once after NR for both presentation paths.
		if (NeuralRendering::UsesReconstructionContract(feature_.Build()) && options.tuning.uiCorrection &&
			FAILED(Interop::RecordCopy(list, featureColor, featureOutput))) {
			status_ = "NR correction background copy rejected"; return false;
		}
		constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		for (auto* input : { featureMotion, featureDepth, featureUI, hudless, composed }) { Transition(list, input, D3D12_RESOURCE_STATE_COMMON, read); }
		if (featureColor != hudless) { Transition(list, featureColor, D3D12_RESOURCE_STATE_COMMON, read); }
		Transition(list, featureOutput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		NeuralRendering::FeatureSession::EvaluationInput input;
		input.commandList = list; input.color = featureColor; input.motionVectors = featureMotion; input.depth = featureDepth;
		input.output = featureOutput; input.ui = featureUI;
		// This runtime aliases Backbuffer to output in UAV state. The older
		// runtime reads the composed input instead.
		input.backbuffer = NeuralRendering::UsesReconstructionContract(feature_.Build()) ? featureOutput : composed ? composed : hudless;
		// Uniform mode retains the existing runtime guide/scale contract. Packed
		// guides already express endpoint displacement in model pixels.
		const float motionRatio = resolving ? float(constants.workWidth) / constants.sourceWidth : 1.0f;
		input.motionVectorScaleX = reconstruction.peripheralCompression ? 1.0f : scaleX * motionRatio;
		input.motionVectorScaleY = reconstruction.peripheralCompression ? 1.0f : scaleY * motionRatio;
		input.reset = reset; input.depthInverted = depthInverted; input.tuning = options.tuning;
		const bool gpuTiming = timestampHeap_ && timestampReadback_ && mappedTimestamps_ && timestampFrequency_;
		const auto query = static_cast<UINT>(slot * 2);
		if (gpuTiming) { list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query); }
		const auto cpuBegin = std::chrono::steady_clock::now();
		bool evaluated = feature_.RecordEvaluation(input);
		if (!evaluated) { status_ = feature_.Status(); }
		if (evaluated && options.passes == 2) {
			// Each temporal feature sees one evaluation per frame. Feed the first
			// result into a distinct output; never evaluate the same history twice.
			Transition(list, featureOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			if (options.tuning.uiCorrection &&
				FAILED(Interop::RecordCopy(list, featureOutput, secondOutput_.Get()))) {
				status_ = "NR second-pass background copy rejected"; return false;
			}
			Transition(list, featureOutput, D3D12_RESOURCE_STATE_COMMON, read);
			Transition(list, secondOutput_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			auto second = input;
			second.color = featureOutput;
			second.output = secondOutput_.Get();
			second.backbuffer = NeuralRendering::UsesReconstructionContract(secondFeature_.Build()) ? second.output : input.backbuffer;
			second.reset = reset || secondFeature_.EvaluationsRecorded() == 0;
			evaluated = secondFeature_.RecordEvaluation(second);
			if (!evaluated) { status_ = "NR second pass: " + secondFeature_.Status(); }
		}
		const auto cpuRecordNanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - cpuBegin).count());
		if (!evaluated) {
			telemetry_.RecordCPUOnly(cpuRecordNanoseconds);
			return false;
		}
		if (gpuTiming) {
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
			list->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 2,
				timestampReadback_.Get(), sizeof(std::uint64_t) * query);
			pendingTiming_[slot] = { feature_.EvaluationsRecorded(), cpuRecordNanoseconds, true };
		} else {
			telemetry_.RecordCPUOnly(cpuRecordNanoseconds);
		}
		if (options.passes == 2) {
			Transition(list, featureOutput, read, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, secondOutput_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			// Keep the existing resolve/composition endpoint stable. Resolve runs
			// once, comparing the original input with the final NR result.
			if (FAILED(Interop::RecordCopy(list, secondOutput_.Get(), featureOutput))) {
				status_ = "NR second-pass result copy rejected"; return false;
			}
			Transition(list, featureOutput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
		if (resolving) {
			Transition(list, featureOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			if (featureColor != hudless) { Transition(list, featureColor, read, D3D12_RESOURCE_STATE_COMMON); }
			for (auto* resource : { featureMotion, featureDepth, featureUI, hudless, composed }) { Transition(list, resource, read, D3D12_RESOURCE_STATE_COMMON); }
			if (method == NeuralRendering::ResolveMethod::Residual) {
				constants.targetWidth = constants.workWidth; constants.targetHeight = constants.workHeight;
				if (!dispatch(2, ResolveKernel::Residual, featureColor, featureOutput, nullptr, residual_.Get())) { return false; }
				constants.mode = 1;
				if (!dispatch(3, ResolveKernel::Residual, hudless, residual_.Get(), nullptr, corrected_.Get())) { return false; }
			} else {
				constants.mode = 1; constants.targetWidth = constants.sourceWidth; constants.targetHeight = constants.sourceHeight;
				if (!dispatch(3, ResolveKernel::Ratio, featureColor, featureOutput, hudless, corrected_.Get())) { return false; }
			}
			for (auto* resource : { featureMotion, featureDepth, featureUI, hudless, composed }) { Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, read); }
			Transition(list, corrected_.Get(), D3D12_RESOURCE_STATE_COMMON, read);
		} else {
			Transition(list, corrected_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
		}

		if (options.WorldOnly()) {
			Transition(list, corrected_.Get(), read, D3D12_RESOURCE_STATE_COMMON);
			for (auto* texture : { featureMotion, featureDepth, hudless }) { Transition(list, texture, read, D3D12_RESOURCE_STATE_COMMON); }
			status_ = std::format("NR {} upscaling {}x{} -> {}x{}; {} pass(es); {}; {}; world only; UI correction unused; fusion requested={} colour={} guides={}",
				options.beforeUpscaling ? "before" : "after", constants.workWidth, constants.workHeight,
				constants.sourceWidth, constants.sourceHeight, options.passes,
				reconstruction.producerColor ? "producer RGB reconstruction" : "user reconstruction",
				reconstruction.peripheralCompression ? "peripheral 80/90" : "uniform",
				reconstruction.fusedPreparation, fusedColor_, reconstruction.fusedPreparation && reconstruction.peripheralCompression);
			return true;
		}

		// Interop::Begin retired this slot before any descriptors are
		// overwritten. NGX may bind its own heaps/root/PSO; explicitly bind ours.
		// In peripheral mode NGX consumed a private warped UI. Composition still
		// samples the untouched native UI; it has remained COMMON until here.
		if (featureUI != ui) { Transition(list, ui, D3D12_RESOURCE_STATE_COMMON, read); }
		auto* heap = heaps_[slot].Get();
		auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
		const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		for (auto* texture : { corrected_.Get(), ui }) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = texture->GetDesc().Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(texture, &srv, cpu); cpu.ptr += stride;
		}
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = composed_->GetDesc().Format;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(composed_.Get(), nullptr, &uav, cpu);
		Transition(list, composed_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		list->SetDescriptorHeaps(1, &heap); list->SetComputeRootSignature(root_.Get());
		list->SetPipelineState(pipeline_.Get()); list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
		list->Dispatch((static_cast<UINT>(composed_->GetDesc().Width) + 7) / 8, (composed_->GetDesc().Height + 7) / 8, 1);
		Transition(list, composed_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		Transition(list, corrected_.Get(), read, D3D12_RESOURCE_STATE_COMMON);
		for (auto* texture : { featureMotion, featureDepth, featureUI, hudless, composed }) { Transition(list, texture, read, D3D12_RESOURCE_STATE_COMMON); }
		if (featureUI != ui) { Transition(list, ui, read, D3D12_RESOURCE_STATE_COMMON); }
		// Preserve the HUD-less tag identity already registered with Streamline.
		// Both its generated frames and our real-frame composition use corrected_.
		if (FAILED(Interop::RecordCopy(list, corrected_.Get(), hudless))) { status_ = "NR HUD-less copy rejected"; return false; }
		status_ = std::format("source NR after DLSS {}x{} -> {}x{}; {} pass(es); {} resolve; {}; native UI after NR; NR feeds real output and HUD-less FG tag; fusion requested={} colour={} guides={}",
			constants.workWidth, constants.workHeight, constants.sourceWidth, constants.sourceHeight,
			options.passes,
			method == NeuralRendering::ResolveMethod::Ratio ? "ratio" : method == NeuralRendering::ResolveMethod::Residual ? "residual" : "direct",
			reconstruction.peripheralCompression ? "peripheral 80/90" : "uniform",
			reconstruction.fusedPreparation, fusedColor_, reconstruction.fusedPreparation && reconstruction.peripheralCompression);
		return true;
	}
}
