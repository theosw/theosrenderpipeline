#include <PCH.h>
#include "SourceDLSSGNeuralRendering.h"
#include "TRPNeuralShaders.generated.h"
#include <chrono>

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
		// Issues transitions recorded together as one ResourceBarrier call. Null
		// resources are skipped; a resource must appear at most once per flush.
		class Barriers
		{
		public:
			explicit Barriers(ID3D12GraphicsCommandList* list) : list_(list) {}
			Barriers& Add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
			{
				if (!resource) { return *this; }
				if (count_ == barriers_.size()) { Flush(); }
				auto& b = barriers_[count_++];
				b = {};
				b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
				return *this;
			}
			void Flush()
			{
				if (count_) { list_->ResourceBarrier(count_, barriers_.data()); }
				count_ = 0;
			}
		private:
			ID3D12GraphicsCommandList* list_;
			std::array<D3D12_RESOURCE_BARRIER, 8> barriers_{};
			UINT count_{};
		};
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
		query.Count = static_cast<UINT>(kCommandSlots * 4);
		if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&timestampHeap_)))) {
			timestampFrequency_ = 0;
			logger::warn("[DLSSNR Source] GPU boundary timing unavailable: timestamp heap creation failed");
			return;
		}
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = sizeof(std::uint64_t) * kCommandSlots * 4;
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
		const auto query = slot * 4;
		if (pending.modelRan) {
			telemetry_.Record(pending.evaluation, mappedTimestamps_[query],
				mappedTimestamps_[query + 1], timestampFrequency_, pending.cpuRecordNanoseconds);
		}
		telemetry_.RecordFrame(mappedTimestamps_[query + 2], mappedTimestamps_[query + 3], timestampFrequency_);
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
		// Each extra pass's own model raster; a different one resizes into it and back.
		struct ExtraPlan { NeuralRendering::ExtraPassSettings settings; UINT width{}, height{}; bool resize{}; };
		const auto extraPasses = static_cast<std::size_t>(std::clamp(options.passes, 1, NeuralRendering::kMaxNeuralPasses) - 1);
		std::array<ExtraPlan, NeuralRendering::kExtraPasses> plans{};
		for (std::size_t i = 0; i < extraPasses; ++i) {
			auto& plan = plans[i];
			plan.settings = options.EffectiveExtra(i);
			auto extent = reconstruction;
			extent.inputScale = plan.settings.inputScale;
			plan.width = NeuralRendering::ModelExtent(static_cast<UINT>(sceneDesc.Width), extent);
			plan.height = NeuralRendering::ModelExtent(sceneDesc.Height, extent);
			plan.resize = plan.width != workWidth || plan.height != workHeight;
		}
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
			for (std::size_t i = 0; i < extraPasses; ++i) {
				const auto& plan = plans[i];
				auto& extra = extra_[i];
				auto extraDesc = sceneDesc; extraDesc.Width = plan.width; extraDesc.Height = plan.height;
				if (!create(extraDesc, extra.output)) { return false; }
				if (plan.resize) {
					if (!check(resolve_.Initialize(device), "NR extra-pass resize kernels") || !create(extraDesc, extra.input)) { return false; }
					auto restoredDesc = sceneDesc; restoredDesc.Width = workWidth; restoredDesc.Height = workHeight;
					if (!create(restoredDesc, extra.restored)) { return false; }
					if (reconstruction.peripheralCompression) {
						auto guideDesc = extraDesc; guideDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
						if (!create(guideDesc, extra.motion)) { return false; }
						guideDesc.Format = DXGI_FORMAT_R32_FLOAT;
						if (!create(guideDesc, extra.depth)) { return false; }
						if (!options.WorldOnly() && !create(extraDesc, extra.ui)) { return false; }
					}
				}
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
				} else if (!fusedColor_ && (reconstruction.colorIsHDR || reconstruction.producerColor ||
					(options.beforeUpscaling && method == NeuralRendering::ResolveMethod::Ratio)) && !create(sceneDesc, encoded_)) { return false; }
			}
			if (!options.WorldOnly()) {
				D3D12_DESCRIPTOR_RANGE ranges[2]{};
				ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
				ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2 };
				D3D12_ROOT_PARAMETER parameter{};
				parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				parameter.DescriptorTable = { 2, ranges };
				D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.NumParameters = 1; rootDesc.pParameters = &parameter;
				Microsoft::WRL::ComPtr<ID3DBlob> signature, errors;
				if (!check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
					&signature, &errors), "NR compose root serialization") ||
					!check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
						IID_PPV_ARGS(&root_)), "NR compose root")) { return false; }
				D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root_.Get();
				pso.CS = { CompiledNeuralShaders::Compose.data, CompiledNeuralShaders::Compose.size };
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
		info.displayWidth = workWidth; info.displayHeight = workHeight;
		info.renderWidth = reconstruction.peripheralCompression ? workWidth : static_cast<UINT>(motion->GetDesc().Width);
		info.renderHeight = reconstruction.peripheralCompression ? workHeight : motion->GetDesc().Height;
		// Each identified runtime contract selects its own default preset.
		info.networkPreset = reconstruction.preset == 0 ? -1 : reconstruction.preset;
		if (!feature_.EnsureInitialized(info)) { status_ = feature_.Status(); return false; }
		// Pass 1 and 2 start together as before; Passes 3 to 5 follow one per frame.
		auto ramp = NeuralRendering::NextFeatureRamp(createdPasses_, options.passes, startablePasses_);
		for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(ramp.created); ++i) {
			const auto& plan = plans[i];
			auto& feature = extra_[i].feature;
			info.displayWidth = plan.width; info.displayHeight = plan.height;
			info.networkPreset = plan.settings.preset == 0 ? -1 : plan.settings.preset;
			if (reconstruction.peripheralCompression) { info.renderWidth = plan.width; info.renderHeight = plan.height; }
			if (!feature.EnsureInitialized(info)) {
				// NGX refusing a later pass is not a reason to drop the runtime: the
				// passes before it keep running, and this one is not tried again
				// until NR is recreated. Nothing was recorded for it.
				if (feature.RefusedCreation()) {
					const int running = static_cast<int>(i) + 1;
					startablePasses_ = running;
					ramp.created = running;
					ramp.ready = (std::min)(ramp.ready, running);
					passNote_ = std::format("Pass {} could not start: {}; running {} pass{}", running + 1,
						feature.Status(), running, running == 1 ? "" : "es");
					logger::warn("[DLSSNR Source] {}", passNote_);
					break;
				}
				status_ = std::format("NR pass {} feature: {}", i + 2, feature.Status()); return false;
			}
			if (plan.resize && !NeuralRendering::UsesReconstructionContract(feature.Build())) {
				status_ = "Independent NR pass resolutions require the reconstruction runtime"; return false;
			}
		}
		// Model rate evaluates on a frame that created a feature.
		createdThisFrame_ = ramp.created != createdPasses_;
		createdPasses_ = ramp.created;
		readyPasses_ = ramp.ready;
		for (std::size_t i = 0; i < extraPasses; ++i) { extra_[i].settings = plans[i].settings; }
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

#include "SourceDLSSGNeuralSecondPass.inl"

	bool NeuralPass::EnsureModelRate(ID3D12Device* device, ID3D12Resource* motion, ID3D12Resource* hudless)
	{
		if (const auto hr = resolve_.Initialize(device); FAILED(hr)) {
			status_ = std::format("NR model-rate kernels failed 0x{:08X}", static_cast<UINT>(hr)); return false;
		}
		// The pass is recreated when the guides or the scene change size.
		if (rateChange_) {
			if (!SameSize(rateChange_.Get(), hudless) || !SameSize(rateMotion_.Get(), motion)) {
				status_ = "NR model-rate textures changed size without retiring the pass"; return false;
			}
			return true;
		}
		auto create = [&](ID3D12Resource* like, DXGI_FORMAT format, Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = like->GetDesc().Width; desc.Height = like->GetDesc().Height;
			desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
			desc.Format = format; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)))) {
				status_ = "NR model-rate allocation failed"; return false;
			}
			return true;
		};
		// RGBA16F guides and change (see the shader's notes on precision); the
		// composed motion is RG32F like the packed guides the model already reads.
		return create(motion, DXGI_FORMAT_R16G16B16A16_FLOAT, rateGuides_[0]) &&
			create(motion, DXGI_FORMAT_R16G16B16A16_FLOAT, rateGuides_[1]) &&
			create(motion, DXGI_FORMAT_R32G32_FLOAT, rateMotion_) &&
			create(hudless, DXGI_FORMAT_R16G16B16A16_FLOAT, rateChange_);
	}

	bool NeuralPass::HoldFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, bool freeze,
		ID3D12Resource*& motion, ID3D12Resource*& depth, ID3D12Resource* hudless, ID3D12Resource*& objectMask, bool& reset)
	{
		if (!freeze) {
			// Release the hold. The model's history is of the held frame. The held
			// textures stay allocated: lists in flight may still read them, and the
			// pass's retirement releases them with everything else.
			if (frozen_) { reset = true; }
			frozen_ = false;
			return true;
		}
		auto allocate = [&](ID3D12Resource* like, Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
			if (out) { return true; }
			// Committed default-heap memory starts zeroed; stillMotion_ relies on it.
			auto desc = like->GetDesc();
			D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)))) {
				status_ = "NR freeze-frame allocation failed"; return false;
			}
			return true;
		};
		if (!allocate(hudless, heldColor_) || !allocate(depth, heldDepth_) || !allocate(motion, stillMotion_) ||
			(objectMask && !allocate(objectMask, heldMask_))) { return false; }
		auto copy = [&](ID3D12Resource* source, ID3D12Resource* destination) {
			if (FAILED(Interop::RecordCopy(list, source, destination))) { status_ = "NR freeze-frame copy rejected"; return false; }
			return true;
		};
		if (!frozen_) {
			// Take this frame. A mask that appears later is held from its first frame.
			if (!copy(hudless, heldColor_.Get()) || !copy(depth, heldDepth_.Get()) ||
				(objectMask && !copy(objectMask, heldMask_.Get()))) { return false; }
			frozen_ = true; reset = true;
		}
		// The scene input is rewritten in place: everything after NR, including
		// the HUD-less tag and the comparison, sees the held frame.
		if (!copy(heldColor_.Get(), hudless)) { return false; }
		motion = stillMotion_.Get(); depth = heldDepth_.Get();
		if (objectMask) { objectMask = heldMask_.Get(); }
		return true;
	}

	bool NeuralPass::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
		const NeuralOptions& options, bool reset, bool depthInverted, float scaleX, float scaleY,
		ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* ui,
		ID3D12Resource* hudless, ID3D12Resource* composed, std::uint64_t timestampFrequency,
		const D3D12StageMarks& marks, ID3D12Resource* objectMask, std::array<float,2> objectMaskGuideOffset,
		std::array<float,2> colorJitter)
	{
		if (!device || !list || slot >= heaps_.size() || options.passes < 1 || options.passes > NeuralRendering::kMaxNeuralPasses || !SameSize(motion, depth) ||
			!Texture(hudless) || (options.WorldOnly() ? (ui || composed || options.tuning.uiCorrection) :
			(!SameSize(hudless, composed) || !SameSize(ui, hudless) || hudless == composed)) ||
			!std::isfinite(scaleX) || !std::isfinite(scaleY) || !scaleX || !scaleY) {
			status_ = "NR source input contract is incomplete"; return false;
		}
		if (!Initialize(device, options, motion, hudless, composed)) { return false; }
		// Before upscaling the held frame would reach DLSS without its jitter.
		if (!HoldFrame(device, list, options.compare.freeze && !options.beforeUpscaling, motion, depth, hudless, objectMask, reset)) {
			return false;
		}
		// A pass whose feature was created this frame waits for the next one.
		const int effectivePasses = (std::max)(1, (std::min)(options.EffectivePasses(), readyPasses_));
		for (std::size_t i = static_cast<std::size_t>(effectivePasses) - 1; i < extra_.size(); ++i) { extra_[i].historyInvalid = true; }
		InitializeTelemetry(device, timestampFrequency);
		// Interop::Begin retired this exact command slot before Record was called.
		// Reading its previous query pair therefore never adds a CPU wait.
		HarvestTelemetry(slot);
		const bool gpuTiming = timestampHeap_ && timestampReadback_ && mappedTimestamps_ && timestampFrequency_;
		const auto query = static_cast<UINT>(slot * 4);
		if (gpuTiming) { list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 2); }
		std::uint64_t modelCPUTime{};
		bool modelRecorded{};
		auto finishTiming = [&] {
			if (!gpuTiming) { return; }
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 3);
			// Skipped frames never wrote the inference pair; do not resolve/read it.
			const bool ran = modelRecorded;
			list->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + (ran ? 0 : 2), ran ? 4 : 2,
				timestampReadback_.Get(), sizeof(std::uint64_t) * (query + (ran ? 0 : 2)));
			pendingTiming_[slot] = { feature_.EvaluationsRecorded(), modelCPUTime, true, ran };
		};
		// Marks left open by a failed recording are dropped with the list.
		marks.Begin(D3D12Stage::kNeuralPrepare);
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
		// The passes whose object controls run this frame. A change resets their histories.
		std::array<bool, NeuralRendering::kMaxNeuralPasses> controlWanted{};
		bool controlReset = false;
		for (unsigned pass = 0; pass < controlWanted.size(); ++pass) {
			controlWanted[pass] = objectMask && pass < unsigned(effectivePasses) &&
				NeuralRendering::UsesReconstructionContract(feature_.Build()) && options.objectMasks.passes[pass].Active();
			controlReset |= controlWanted[pass] != objectControlActive_[pass];
		}

		// Model rate: decide before preparing anything for the model (NeuralModelRate.h).
		const auto rateNow = std::chrono::steady_clock::now();
		// Frames NR did not record (a menu, a stall) break the motion chain.
		const bool gap = rateTime_ != std::chrono::steady_clock::time_point{} && rateNow - rateTime_ > std::chrono::milliseconds(100);
		rateTime_ = rateNow;
		const bool rateChanged = gap || createdThisFrame_ || effectivePasses != rateEffectivePasses_ ||
			!SameModelOutputOptions(options, rateOptions_);
		rateOptions_ = options; rateEffectivePasses_ = effectivePasses;
		const auto rate = rateSchedule_.Next({ NeuralRendering::EffectiveModelRate(reconstruction.modelRate), reset || controlReset, rateChanged, frozen_ });
		const bool skipped = !rate.evaluate;
		telemetry_.RecordModelRun(rate.evaluate);
		// Camera jitter in render pixels. Before upscaling the colour raster carries
		// it; after upscaling only the guides do, against the steady output.
		const auto cameraJitter = options.beforeUpscaling ? colorJitter : objectMaskGuideOffset;
		const bool jitterKnown = std::isfinite(cameraJitter[0]) && std::isfinite(cameraJitter[1]);
		constants.cameraJitterX = jitterKnown ? cameraJitter[0] : 0; constants.cameraJitterY = jitterKnown ? cameraJitter[1] : 0;
		constants.previousJitterX = rateJitter_[0]; constants.previousJitterY = rateJitter_[1];
		constants.guidesJittered = !options.beforeUpscaling;
		constants.depthInverted = depthInverted;
		constants.categories = objectMask && rateMask_;
		// Multiplicative above, additive below: about 5/255 of display-encoded
		// colour, or a deep shadow of linear HDR relative to its white point.
		constants.changeFloor = reconstruction.producerColor ? 0.02f * reconstruction.whitePoint :
			reconstruction.colorIsHDR ? 0.002f * reconstruction.whitePoint : 0.02f;
		ID3D12Resource* currentGuides{};
		ID3D12Resource* previousGuides{};
		if (rate.captureGuides || rate.storeChange) {
			if (!EnsureModelRate(device, motion, hudless)) { return false; }
		}
		if (rate.captureGuides) {
			currentGuides = rateGuides_[rateGuide_].Get(); previousGuides = rateGuides_[rateGuide_ ^ 1].Get();
			if (!dispatch(NeuralResolveKernels::ModelRateStage(0), ResolveKernel::CaptureGuides, motion, depth, objectMask, currentGuides)) {
				return false;
			}
			rateGuide_ ^= 1;
			rateJitter_ = { constants.cameraJitterX, constants.cameraJitterY };
			rateMask_ = objectMask != nullptr;
		}
		if (rate.composeMotion) {
			// Every pass's history is two frames old; packing and the extra passes
			// read this composed field as the frame's motion.
			if (!dispatch(NeuralResolveKernels::ModelRateStage(1), ResolveKernel::ComposeMotion, motion, currentGuides, previousGuides,
				rateMotion_.Get())) { return false; }
			motion = rateMotion_.Get();
			featureMotion = motion;
		}
		// Every resource NGX reads. featureColor is hudless unless preparation replaced it.
		static_assert(NeuralRendering::kMaxNeuralPasses == 5, "list every pass's control mask");
		std::array<ID3D12Resource*, 6 + NeuralRendering::kMaxNeuralPasses> inputs{};
		auto transitionInputs = [&](Barriers& barriers, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			for (auto* resource : inputs) { barriers.Add(resource, before, after); }
		};
		constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
		constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		if (skipped) {
			// No model on this frame: carry the previous frame's change onto this
			// input. Nothing NR's passes would prepare is needed; everything after
			// corrected_ runs as on an evaluated frame.
			marks.End(D3D12Stage::kNeuralPrepare);
			marks.Begin(D3D12Stage::kNeuralFinish);
			if (const auto hr = resolve_.Record(device, list, slot, NeuralResolveKernels::ModelRateStage(3), ResolveKernel::Reproject,
				constants, hudless, rateChange_.Get(), currentGuides, corrected_.Get(), nullptr, previousGuides); FAILED(hr)) {
				status_ = std::format("NR model-rate reprojection failed 0x{:08X}", static_cast<UINT>(hr)); return false;
			}
			if (rate.storeChange) {
				// The next skip reads this frame's guides, so its change must be
				// carried onto this frame too, before previews or UI touch it.
				if (!dispatch(NeuralResolveKernels::ModelRateStage(2), ResolveKernel::StoreChange, hudless, corrected_.Get(), nullptr,
					rateChange_.Get())) { return false; }
			}
			if (rate.accumulateMotion) {
				// Every third frame, second skip: reprojection above moved the stored
				// change one frame. The next evaluation's history is three frames old,
				// so this frame's guides keep the motion back to the last evaluation.
				if (!dispatch(NeuralResolveKernels::ModelRateStage(4), ResolveKernel::ComposeMotion, motion, currentGuides,
						previousGuides, rateMotion_.Get()) ||
					!dispatch(NeuralResolveKernels::ModelRateStage(5), ResolveKernel::CaptureGuides, rateMotion_.Get(), depth,
						objectMask, currentGuides)) { return false; }
			}
		} else {
			// Adaptive exposure measures the original scene before anything encodes it.
			const bool adaptive = resolving && reconstruction.adaptiveExposure && reconstruction.colorIsHDR &&
				!reconstruction.producerColor;
			constants.adaptiveExposure = adaptive;
			constants.exposureBias = reconstruction.exposureBias;
			if (adaptive) {
				if (!exposure_) {
					D3D12_RESOURCE_DESC desc{};
					desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = desc.Height = 1;
					desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
					desc.Format = DXGI_FORMAT_R32_FLOAT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
					D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
					if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
						D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&exposure_)))) {
						status_ = "NR exposure allocation failed"; return false;
					}
				}
				// Half the remaining difference every quarter second: about a second to settle.
				const auto now = std::chrono::steady_clock::now();
				const float seconds = std::clamp(std::chrono::duration<float>(now - exposureTime_).count(), 0.0f, 0.25f);
				constants.exposureRate = 1.0f - std::exp2(-seconds / 0.25f);
				constants.exposureReset = reset || !exposureValid_;
				resolve_.SetExposure(exposure_.Get());
				if (!dispatch(12, ResolveKernel::MeasureExposure, hudless, nullptr, nullptr, exposure_.Get())) { return false; }
				exposureTime_ = now; exposureValid_ = true;
				constants.exposureReset = false;
			} else {
				resolve_.SetExposure(nullptr);
				exposureValid_ = false;
			}
			// Before upscaling, Ratio shows NR the frame on the steady grid and moves its
			// change back to the jittered one; DLSS still receives the jittered pixels.
			// Only the passes that read the raw frame, and the final resolve, use it.
			const bool steady = method == NeuralRendering::ResolveMethod::Ratio &&
				options.beforeUpscaling && std::isfinite(colorJitter[0]) && std::isfinite(colorJitter[1]) &&
				(colorJitter[0] != 0 || colorJitter[1] != 0);
			const auto setJitter = [&](bool on) {
				constants.jitterX = on ? colorJitter[0] : 0; constants.jitterY = on ? colorJitter[1] : 0;
			};
			if (resolving) {
				if (encoded_) {
					constants.targetWidth = constants.sourceWidth; constants.targetHeight = constants.sourceHeight;
					setJitter(steady);
					if (!dispatch(0, ResolveKernel::Ratio, hudless, nullptr, nullptr, encoded_.Get())) { return false; }
					setJitter(false);
					featureColor = encoded_.Get();
				}
				if (workColor_) {
					constants.targetWidth = constants.workWidth; constants.targetHeight = constants.workHeight;
					setJitter(steady && featureColor == hudless);
					if (!dispatch(1, fusedColor_ ? ResolveKernel::PrepareColor : ResolveKernel::Downsample, featureColor, nullptr, nullptr, workColor_.Get())) { return false; }
					setJitter(false);
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
			// Prepare every pass before transitioning shared guides for evaluation.
			// Each mask samples the raw classification/depth at that model's grid,
			// including peripheral compression and the existing SDK pixel jitter.
			std::array<ID3D12Resource*,NeuralRendering::kMaxNeuralPasses> controls{};
			for (unsigned pass=0;pass<controls.size();++pass) {
				const bool active=controlWanted[pass];
				if(active) {
					auto* output=pass?extra_[pass-1].output.Get():featureOutput;
					const auto hr=objectControl_.Record(device,list,slot,pass,options.objectMasks.passes[pass],
						pass?options.EffectiveExtra(pass-1).tuning:options.tuning,objectMask,depth,
						static_cast<UINT>(output->GetDesc().Width),output->GetDesc().Height,reconstruction.peripheralCompression,objectMaskGuideOffset);
					if(FAILED(hr)){status_=std::format("NR object controls failed 0x{:08X}",static_cast<UINT>(hr));return false;}
					controls[pass]=objectControl_.Output(pass);
				}
				objectControlActive_[pass]=active;
				if(pass){extra_[pass-1].controlMask=controls[pass];}
			}
			inputs = { featureMotion, featureDepth, featureUI, hudless, composed,
				featureColor != hudless ? featureColor : nullptr,controls[0],controls[1],controls[2],controls[3],controls[4] };
			{
				Barriers barriers{list};
				transitionInputs(barriers, common, read);
				barriers.Add(featureOutput, common, write).Flush();
			}
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
			input.reset |= controlReset;
			input.controlMask=controls[0];
			if (input.controlMask) {
				// The provider multiplies mask RGB by these global controls. Absolute
				// pass/group values already live in the mask; do not apply them twice.
				input.tuning.useAutoSkinMask = false;
				input.tuning.intensity = input.tuning.localToneStrength = input.tuning.localStructureStrength = 1;
			}
			marks.End(D3D12Stage::kNeuralPrepare);
			marks.Begin(D3D12Stage::kNeuralInference);
			if (gpuTiming) { list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query); }
			// Strength is source-owned, unlike the provider's tone/structure controls.
			// Blend before the next pass reads this output, retaining that pass's input
			// at zero. Each pass owns its scratch texture and retired-slot descriptors.
			// The existing chain timing includes these blends and extra-pass preparation.
			auto applyObjectStrength = [&](unsigned pass, const NeuralRendering::FeatureSession::EvaluationInput& evaluation) {
				if (!objectMask) { return true; }
				const auto strengths = options.objectMasks.passes[pass].Strengths();
				if (std::all_of(strengths.begin(), strengths.end(), [](float value) { return value == 1; })) { return true; }
				auto& result = passObjectResult_[pass];
				if (!result) {
					D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
					const auto desc = evaluation.output->GetDesc();
					const auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
						common, nullptr, IID_PPV_ARGS(&result));
					if (FAILED(hr)) { status_ = "NR per-pass object-result allocation failed"; return false; }
				}
				// Raw depth remains readable when used directly by the first model;
				// peripheral preparation instead left it COMMON. Other guides stay put.
				const bool rawDepthRead = featureDepth == depth;
				Barriers before{list};
				before.Add(evaluation.color, read, common).Add(evaluation.output, write, common);
				if (rawDepthRead) { before.Add(depth, read, common); }
				before.Flush();
				NeuralRendering::ObjectBlendRequest request;
				request.strength = strengths;
				const auto hr = passObjectBlend_[pass].Record(device, list, slot, request,
					evaluation.color, evaluation.output, objectMask, depth, result.Get(), objectMaskGuideOffset,
					reconstruction.peripheralCompression);
				if (FAILED(hr) || FAILED(Interop::RecordCopy(list, result.Get(), evaluation.output))) {
					status_ = std::format("NR pass {} object-strength composition failed", pass + 1); return false;
				}
				Barriers after{list};
				after.Add(evaluation.color, common, read).Add(evaluation.output, common, write);
				if (rawDepthRead) { after.Add(depth, common, read); }
				after.Flush();
				return true;
			};
			const auto cpuBegin = std::chrono::steady_clock::now();
			std::array<NeuralRendering::FeatureSession::EvaluationInput, NeuralRendering::kExtraPasses> extras;
			// The previous pass's result on Pass 1's grid, which the next pass reads.
			ID3D12Resource* previous = featureOutput;
			bool evaluated = feature_.RecordEvaluation(input);
			if (!evaluated) { status_ = feature_.Status(); }
			if (evaluated) { evaluated = applyObjectStrength(0, input); }
			for (int pass = 1; evaluated && pass < effectivePasses; ++pass) {
				auto& evaluation = extras[pass - 1];
				evaluated = RecordExtra(device, list, slot, pass, options, input, previous, constants, motion, depth, ui, evaluation);
				if (evaluated) { evaluated = applyObjectStrength(pass, evaluation); }
				// Every pass but the last is restored here for the next; the last after timing.
				if (evaluated && pass + 1 < effectivePasses) {
					evaluated = FinishExtra(device, list, slot, pass, input, previous, evaluation, constants, &previous);
				}
			}
			const auto cpuRecordNanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - cpuBegin).count());
			modelCPUTime = cpuRecordNanoseconds;
			if (!evaluated) {
				telemetry_.RecordCPUOnly(cpuRecordNanoseconds);
				return false;
			}
			modelRecorded = true;
			if (gpuTiming) {
				list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
			} else {
				telemetry_.RecordCPUOnly(cpuRecordNanoseconds);
			}
			marks.End(D3D12Stage::kNeuralInference);
			marks.Begin(D3D12Stage::kNeuralFinish);

			// With a resolve to follow, it reads the last pass's result in place.
			auto* modelResult = featureOutput;
			if (effectivePasses > 1 && !FinishExtra(device, list, slot, effectivePasses - 1, input, previous,
				extras[effectivePasses - 2], constants, resolving ? &modelResult : nullptr)) { return false; }

			if (resolving) {
				// The kernels take every input in COMMON and leave it there; nothing
				// after the resolve reads the NGX inputs again.
				Barriers barriers{list};
				transitionInputs(barriers, read, common);
				// A direct extra-pass result and featureOutput are already COMMON.
				if (modelResult == featureOutput) { barriers.Add(featureOutput, write, common); }
				barriers.Flush();
				if (method == NeuralRendering::ResolveMethod::Residual) {
					constants.targetWidth = constants.workWidth; constants.targetHeight = constants.workHeight;
					if (!dispatch(2, ResolveKernel::Residual, featureColor, modelResult, nullptr, residual_.Get())) { return false; }
					constants.mode = 1;
					if (!dispatch(3, ResolveKernel::Residual, hudless, residual_.Get(), nullptr, corrected_.Get())) { return false; }
				} else {
					// Broad and Detail: NR's change in stops at its grid, blurred across
					// regions but not across the proxy's own edges, for the resolve to split.
					const bool bands = !reconstruction.producerColor && reconstruction.ToneBands();
					constants.broad = reconstruction.broadStrength; constants.detail = reconstruction.detailStrength;
					constants.toneBands = bands;
					if (bands) {
						const auto work = featureColor->GetDesc();
						for (auto* tone : { std::addressof(toneData_), std::addressof(toneScratch_) }) {
							if (*tone) { continue; }
							D3D12_RESOURCE_DESC desc{};
							desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = work.Width; desc.Height = work.Height;
							desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
							desc.Format = DXGI_FORMAT_R16G16_FLOAT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
							D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
							if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
								D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(tone->ReleaseAndGetAddressOf())))) {
								status_ = "NR broad/detail allocation failed"; return false;
							}
						}
						constants.targetWidth = static_cast<UINT>(work.Width); constants.targetHeight = work.Height;
						if (!dispatch(13, ResolveKernel::PrepareTone, featureColor, modelResult, nullptr, toneData_.Get())) { return false; }
						constants.mode = 0;
						if (!dispatch(14, ResolveKernel::FilterTone, toneData_.Get(), nullptr, nullptr, toneScratch_.Get())) { return false; }
						constants.mode = 1;
						if (!dispatch(15, ResolveKernel::FilterTone, toneScratch_.Get(), nullptr, nullptr, toneData_.Get())) { return false; }
						resolve_.SetTone(toneData_.Get());
					}
					constants.mode = 1; constants.targetWidth = constants.sourceWidth; constants.targetHeight = constants.sourceHeight;
					setJitter(steady);
					if (!dispatch(3, ResolveKernel::Ratio, featureColor, modelResult, hudless, corrected_.Get())) { return false; }
					setJitter(false);
					resolve_.SetTone(nullptr);
				}
			}
			if (rate.storeChange) {
				// NR's change for the next frame, before the preview or comparison
				// draws over corrected_. Without a resolve, NGX left corrected_
				// writable and the inputs readable; the kernel takes them in COMMON.
				if (!resolving) {
					Barriers barriers{list};
					barriers.Add(corrected_.Get(), write, common);
					transitionInputs(barriers, read, common);
					barriers.Flush();
				}
				if (!dispatch(NeuralResolveKernels::ModelRateStage(2), ResolveKernel::StoreChange, hudless, corrected_.Get(), nullptr,
					rateChange_.Get())) { return false; }
				if (!resolving) {
					Barriers barriers{list};
					barriers.Add(corrected_.Get(), common, write);
					transitionInputs(barriers, common, read);
					barriers.Flush();
				}
			}
		}
		// NGX wrote corrected_ itself and still holds its inputs readable. A
		// resolve or a frame without the model left everything in COMMON.
		const bool ngxStates = !resolving && !skipped;

		StatusKey key;
		key.workWidth = constants.workWidth; key.workHeight = constants.workHeight;
		key.sourceWidth = constants.sourceWidth; key.sourceHeight = constants.sourceHeight;
		key.passes = effectivePasses; key.worldOnly = options.WorldOnly(); key.beforeUpscaling = options.beforeUpscaling;
		key.producerColor = reconstruction.producerColor; key.peripheral = reconstruction.peripheralCompression;
		key.fusionRequested = reconstruction.fusedPreparation; key.fusedColor = fusedColor_; key.method = method;
		for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(effectivePasses); ++i) {
			const auto& extra = extra_[i];
			key.extra[i] = { static_cast<UINT>(extra.output->GetDesc().Width), extra.output->GetDesc().Height,
				extra.settings.preset, options.extraPasses[i].linked, extra.settings.tuning.intensity };
		}
		key.passOverride = options.passOverride; key.requestedPasses = options.passes; key.readyPasses = readyPasses_;
		key.startablePasses = startablePasses_;
		key.modelInterval = NeuralRendering::ModelRateInterval(NeuralRendering::EffectiveModelRate(reconstruction.modelRate));
		key.objectControls=objectControlActive_;
		key.objectControlsUnavailable=
			std::any_of(options.objectMasks.passes.begin(),options.objectMasks.passes.begin()+options.passes,
				[](const auto& pass){return pass.Active();})&&
			!NeuralRendering::UsesReconstructionContract(feature_.Build());
		// The mask preview is drawn over the finished result at full strength;
		// each pass has already applied its own object strength.
		if (options.objectMasks.previewGroups && objectMask) {
			if (!objectResult_) {
				auto desc = corrected_->GetDesc();
				D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
				const auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&objectResult_));
				if (FAILED(hr)) { status_ = "NR object-result allocation failed"; return false; }
			}
			// The blend and its copy take corrected_ and the inputs in COMMON. A resolve
			// already left them there; otherwise NGX left corrected_ writable and the
			// inputs readable, and the code below expects those states back.
			if (ngxStates) {
				Barriers barriers{list};
				barriers.Add(corrected_.Get(), write, common);
				transitionInputs(barriers, read, common);
				barriers.Flush();
			}
			NeuralRendering::ObjectBlendRequest preview;
			preview.previewGroups = options.objectMasks.previewGroups; preview.classesOnly = options.objectMasks.previewClassesOnly;
			const auto hr = objectBlend_.Record(device, list, slot, preview, hudless, corrected_.Get(), objectMask, depth,
				objectResult_.Get(), objectMaskGuideOffset);
			if (FAILED(hr)) { status_ = std::format("NR object-mask preview failed 0x{:08X}", static_cast<UINT>(hr)); return false; }
			const auto copied = Interop::RecordCopy(list, objectResult_.Get(), corrected_.Get());
			if (FAILED(copied)) { status_ = std::format("NR object-mask preview copy failed 0x{:08X}", static_cast<UINT>(copied)); return false; }
			if (ngxStates) {
				Barriers barriers{list};
				barriers.Add(corrected_.Get(), common, write);
				transitionInputs(barriers, common, read);
				barriers.Flush();
			}
		}
		if (options.compare.split) {
			// Like the preview, the comparison takes corrected_ and the inputs in COMMON.
			if (!splitResult_) {
				auto desc = corrected_->GetDesc();
				D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
				const auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&splitResult_));
				if (FAILED(hr)) { status_ = "NR compare allocation failed"; return false; }
			}
			if (const auto hr = resolve_.Initialize(device); FAILED(hr)) {
				status_ = std::format("NR compare kernels failed 0x{:08X}", static_cast<UINT>(hr)); return false;
			}
			if (ngxStates) {
				Barriers barriers{list};
				barriers.Add(corrected_.Get(), write, common);
				transitionInputs(barriers, read, common);
				barriers.Flush();
			}
			ResolveConstants split;
			split.targetWidth = split.sourceWidth = static_cast<UINT>(corrected_->GetDesc().Width);
			split.targetHeight = split.sourceHeight = corrected_->GetDesc().Height;
			split.splitX = NeuralRendering::SplitColumn(options.compare, split.targetWidth);
			if (const auto hr = resolve_.Record(device, list, slot, 11, ResolveKernel::Split, split, hudless, corrected_.Get(),
				nullptr, splitResult_.Get()); FAILED(hr)) {
				status_ = std::format("NR compare failed 0x{:08X}", static_cast<UINT>(hr)); return false;
			}
			const auto copied = Interop::RecordCopy(list, splitResult_.Get(), corrected_.Get());
			if (FAILED(copied)) { status_ = std::format("NR compare copy failed 0x{:08X}", static_cast<UINT>(copied)); return false; }
			if (ngxStates) {
				Barriers barriers{list};
				barriers.Add(corrected_.Get(), common, write);
				transitionInputs(barriers, common, read);
				barriers.Flush();
			}
		}
		if (options.WorldOnly()) {
			if (ngxStates) {
				// NGX wrote corrected_ directly; return it and its inputs to COMMON.
				Barriers barriers{list};
				barriers.Add(corrected_.Get(), write, common);
				transitionInputs(barriers, read, common);
				barriers.Flush();
			}
			PublishStatus(key);
			marks.End(D3D12Stage::kNeuralFinish);
			finishTiming();
			return true;
		}

		// Interop::Begin retired this slot before any descriptors are
		// overwritten. NGX may bind its own heaps/root/PSO; explicitly bind ours.
		// In peripheral mode NGX consumed a private warped UI. Composition still
		// samples the untouched native UI. After a resolve it is COMMON, as it is
		// when NGX read the warped copy; otherwise it is still in the read state.
		const bool uiCommon = !ngxStates || featureUI != ui;
		{
			Barriers barriers{list};
			barriers.Add(corrected_.Get(), ngxStates ? write : common, read);
			if (uiCommon) { barriers.Add(ui, common, read); }
			barriers.Add(composed_.Get(), common, write).Flush();
		}
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
		list->SetDescriptorHeaps(1, &heap); list->SetComputeRootSignature(root_.Get());
		list->SetPipelineState(pipeline_.Get()); list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
		list->Dispatch((static_cast<UINT>(composed_->GetDesc().Width) + 7) / 8, (composed_->GetDesc().Height + 7) / 8, 1);
		{
			Barriers barriers{list};
			barriers.Add(composed_.Get(), write, common).Add(corrected_.Get(), read, common);
			if (uiCommon) { barriers.Add(ui, read, common); }
			// Without a resolve, the NGX inputs (including a native UI it read) are still readable.
			if (ngxStates) { transitionInputs(barriers, read, common); }
			barriers.Flush();
		}
		// Preserve the HUD-less tag identity already registered with Streamline.
		// Both its generated frames and our real-frame composition use corrected_.
		if (FAILED(Interop::RecordCopy(list, corrected_.Get(), hudless))) { status_ = "NR HUD-less copy rejected"; return false; }
		marks.End(D3D12Stage::kNeuralFinish);
		PublishStatus(key);
		finishTiming();
		return true;
	}

	void NeuralPass::PublishStatus(const StatusKey& key)
	{
		// Status() is read every frame; reformat only when a reported field changes.
		if (successStatus_.empty() || key != statusKey_) {
			statusKey_ = key;
			const auto reconstruction = key.peripheral ? "peripheral 80/90" : "uniform";
			if (key.worldOnly) {
				successStatus_ = std::format("NR {} upscaling {}x{} -> {}x{}; {} pass(es); {}; {}; world only; UI correction unused; fusion requested={} colour={} guides={}",
					key.beforeUpscaling ? "before" : "after", key.workWidth, key.workHeight,
					key.sourceWidth, key.sourceHeight, key.passes,
					key.producerColor ? "producer RGB reconstruction" : "user reconstruction",
					reconstruction, key.fusionRequested, key.fusedColor, key.fusionRequested && key.peripheral);
			} else {
				successStatus_ = std::format("source NR after DLSS {}x{} -> {}x{}; {} pass(es); {} resolve; {}; native UI after NR; NR feeds real output and HUD-less FG tag; fusion requested={} colour={} guides={}",
					key.workWidth, key.workHeight, key.sourceWidth, key.sourceHeight, key.passes,
					key.method == NeuralRendering::ResolveMethod::Ratio ? "ratio" : key.method == NeuralRendering::ResolveMethod::Residual ? "residual" : "direct",
					reconstruction, key.fusionRequested, key.fusedColor, key.fusionRequested && key.peripheral);
			}
			for (int pass = 2; pass <= key.passes; ++pass) {
				const auto& extra = key.extra[pass - 2];
				successStatus_ += std::format("; pass{}={}x{} preset={} linked={} intensity={:.3f}",
					pass, extra.width, extra.height, extra.preset, extra.linked, extra.intensity);
			}
			if (key.passOverride != NeuralRendering::PassOverride::None) {
				successStatus_ += std::format("; pass override={}; requested={}",
					NeuralRendering::PassOverrideName(key.passOverride), key.requestedPasses);
			}
			// A new pass runs from the frame after its feature's creation.
			if (key.startablePasses < key.requestedPasses) {
				successStatus_ += "; " + passNote_;
			} else if (key.readyPasses < key.requestedPasses) {
				successStatus_ += std::format("; starting passes {}-{}", key.readyPasses + 1, key.requestedPasses);
			}
			if(std::ranges::any_of(key.objectControls,std::identity{})){
				// Masked passes only, for example "1,3".
				std::string masked;
				for(std::size_t pass=0;pass<key.objectControls.size();++pass){
					if(key.objectControls[pass]){masked+=std::format("{}{}",masked.empty()?"":",",pass+1);}
				}
				successStatus_+=std::format("; object tone/structure passes={} (auto skin paused on masked passes)",masked);
			}
			if (key.modelInterval == 2) { successStatus_ += "; model every other frame"; }
			else if (key.modelInterval == 3) { successStatus_ += "; model every third frame"; }
			if(key.objectControlsUnavailable){successStatus_+="; object tone/structure unavailable on legacy NR; per-pass and final strength retained";}
		}
		if (status_ != successStatus_) { status_ = successStatus_; }
	}
}
