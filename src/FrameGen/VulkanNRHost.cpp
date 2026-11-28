#include <PCH.h>
#include "VulkanNRHost.h"
#include "D3D12Helpers.h"
#include "NeuralAdmission.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "NeuralRuntimeSession.h"
#include "../PluginPaths.h"
#include "TRPNeuralShaders.generated.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <thread>

namespace TheosRenderPipeline::NeuralRendering
{
	namespace
	{
		constexpr std::string_view kOriginal310ModelSha256 =
			"2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F";

		struct VulkanApi
		{
			HMODULE module{};
			vknr::Query query{};
			vknr::CreateFn create{};
			vknr::Prepare prepare{};
			vknr::Submit submit{};
			vknr::Release release{};
			vknr::LastError error{};
			vknr::Timing timing{};
			vknr::Poll poll{};
			vknr::Drain drain{};

			bool Complete() const
			{
				return query && create && prepare && submit && release && error && timing && poll && drain;
			}

			// Returns whether all exports loaded; sets loadedModule=true if the DLL
			// became resident in the process (even if exports were incomplete).
			bool Load(bool& loadedModule)
			{
				static std::mutex loadMutex;
				std::scoped_lock lock(loadMutex);
				if (module) {
					loadedModule = true;
					return Complete();
				}
				const auto path = VulkanNRHost::Path();
				module = LoadLibraryExW(path.c_str(), nullptr,
					LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
				loadedModule = module != nullptr;
				if (!module) { return false; }
#define TRP_LOAD_VKNR(field, type, name) field = reinterpret_cast<type>(GetProcAddress(module, name))
				TRP_LOAD_VKNR(query, vknr::Query, "vknrQuery");
				TRP_LOAD_VKNR(create, vknr::CreateFn, "vknrCreate");
				TRP_LOAD_VKNR(prepare, vknr::Prepare, "vknrPrepare");
				TRP_LOAD_VKNR(submit, vknr::Submit, "vknrSubmit");
				TRP_LOAD_VKNR(release, vknr::Release, "vknrRelease");
				TRP_LOAD_VKNR(error, vknr::LastError, "vknrLastError");
				TRP_LOAD_VKNR(timing, vknr::Timing, "vknrTiming");
				TRP_LOAD_VKNR(poll, vknr::Poll, "vknrPoll");
				TRP_LOAD_VKNR(drain, vknr::Drain, "vknrDrain");
#undef TRP_LOAD_VKNR
				return Complete();
			}
		};

		VulkanApi g_api;
		std::mutex g_probeMutex;

		const char* PathLabel(vknr::Path path)
		{
			return path == vknr::Path::RDNA3 ? "rdna3" : "portable";
		}
	}

	std::filesystem::path VulkanNRHost::Directory()
	{
#ifdef TRP_NR_VENDOR_BENCHMARK
		wchar_t test[32768]{};
		const auto n = GetEnvironmentVariableW(L"TRP_VKNR_TEST_ROOT", test, 32768);
		if (n && n < 32768) { return test; }
#endif
		return PluginPaths::Directory() / L"TheosRenderPipeline" / L"VulkanNR";
	}

	std::filesystem::path VulkanNRHost::Path()
	{
		return Directory() / L"TRPVulkanNR.dll";
	}

	std::filesystem::path VulkanNRHost::Weights()
	{
#ifdef TRP_NR_VENDOR_BENCHMARK
		wchar_t test[32768]{};
		const auto n = GetEnvironmentVariableW(L"TRP_VKNR_TEST_WEIGHTS", test, 32768);
		if (n && n < 32768) { return test; }
#endif
		return PluginPaths::Directory() / L"TheosRenderPipeline" / L"NVIDIA" / L"nvngx_dlssnr_310.9.16.dll";
	}

	vknr::Capability VulkanNRHost::Probe(ID3D12Device* device, bool wait, bool request)
	{
		if (const auto* reason = runtimeSession.Refusal(Backend::Vulkan)) {
			vknr::Capability result;
			strcpy_s(result.reason, reason);
			return result;
		}
		struct Job
		{
			std::atomic_bool done{};
			vknr::Capability result;
		};
		static std::map<std::uint64_t, std::shared_ptr<Job>> cache;
		std::shared_ptr<Job> job;
		{
			std::scoped_lock lock(g_probeMutex);
			vknr::Capability result;
			if (!device) {
				strcpy_s(result.reason, request ? "Vulkan NR needs the rendering device" : kVulkanNotChecked);
				return result;
			}
			const auto luid = device->GetAdapterLuid();
			const auto key = D3D12Helpers::LuidToUInt64(luid);
			auto found = cache.find(key);
			if (found == cache.end() && !request) {
				strcpy_s(result.reason, kVulkanNotChecked);
				return result;
			}
			auto& entry = cache[key];
			if (!entry) {
				entry = std::make_shared<Job>();
				Microsoft::WRL::ComPtr<ID3D12Device> held = device;
				logger::info("[Vulkan NR] probe requested by explicit Vulkan selection; adapter LUID={:08X}:{:08X}",
					static_cast<std::uint32_t>(luid.HighPart), luid.LowPart);
				try {
					std::thread([pending = entry, held] {
						try {
							std::error_code ec;
							if (const auto* reason = runtimeSession.Refusal(Backend::Vulkan)) {
								strcpy_s(pending->result.reason, reason);
							} else if (!std::filesystem::is_regular_file(Weights(), ec)) {
								strcpy_s(pending->result.reason,
									"Vulkan NR needs Standard's NVIDIA/nvngx_dlssnr_310.9.16.dll. "
									"Install Standard below Universal, or supply that matching runtime.");
							} else if (!std::filesystem::is_regular_file(Path(), ec)) {
								strcpy_s(pending->result.reason, "Vulkan NR engine is missing. Reinstall Universal.");
							} else if (!NeuralRenderingRuntimeIdentity::VerifyExpected(
									Weights(), kPascalRuntimeBytes, kOriginal310ModelSha256).matched) {
								strcpy_s(pending->result.reason,
									"Vulkan NR needs the original NVIDIA 310.9.16 model from Standard; the installed file does not match.");
							} else if (!runtimeSession.BeginVulkanProbe()) {
								strcpy_s(pending->result.reason, kRuntimeRestart);
							} else {
								bool loadedModule = false;
								if (!g_api.Load(loadedModule)) {
									if (!loadedModule) { runtimeSession.ReleaseUnloadedVulkanProbe(); }
									strcpy_s(pending->result.reason,
										loadedModule ? "Vulkan NR engine ABI is incomplete. Reinstall Universal."
										             : "Vulkan NR engine could not be loaded. Reinstall Universal.");
								} else {
									const auto queried = g_api.query(held.Get(), Directory().c_str(), &pending->result,
										[](const char* json) {
											logger::info("[Vulkan NR] probe device capabilities: {}", json);
										});
									if (queried != vknr::Result::Ok) { pending->result.available = false; }
									if (!pending->result.available && !pending->result.reason[0]) {
										strcpy_s(pending->result.reason,
											"Vulkan NR engine ABI/query refused this host. Reinstall the matching Universal package.");
									}
								}
							}
						} catch (...) {
							strcpy_s(pending->result.reason, "Vulkan NR capability query failed");
						}
						const auto& c = pending->result;
						logger::info("[Vulkan NR] probe complete available={} path={} reason={} pathReason={} device={} revision={}; cached for this session",
							c.available, c.available ? PathLabel(c.path) : "refused",
							c.reason, c.pathReason, c.device, c.revision);
						pending->done.store(true, std::memory_order_release);
					}).detach();
				} catch (...) {
					strcpy_s(entry->result.reason, "Vulkan NR capability worker could not start");
					logger::warn("[Vulkan NR] probe refused: {}; cached for this session", entry->result.reason);
					entry->done.store(true, std::memory_order_release);
				}
			}
			job = entry;
		}
		// Only the selected backend's background preparation worker/manual smoke waits.
		if (wait) {
			while (!job->done.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
		if (job->done.load(std::memory_order_acquire)) { return job->result; }
		vknr::Capability pending;
		strcpy_s(pending.reason, kVulkanChecking);
		return pending;
	}

	bool VulkanNRHost::CreateShaders(ID3D12Device* device)
	{
		D3D12_DESCRIPTOR_RANGE ranges[2]{
			{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 },
			{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 3 }
		};
		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[0].DescriptorTable = { 2, ranges };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[1].Constants = { 0, 0, 4 };
		D3D12_ROOT_SIGNATURE_DESC desc{};
		desc.NumParameters = 2;
		desc.pParameters = params;
		Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
		if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
			FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
			return false;
		}
		using namespace SourceDLSSG::CompiledNeuralShaders;
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = root_.Get();
		pso.CS = { VulkanPack, sizeof(VulkanPack) };
		if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pack_)))) { return false; }
		pso.CS = { VulkanUnpack, sizeof(VulkanUnpack) };
		if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&unpack_)))) { return false; }
		for (auto& heap : heaps_) {
			D3D12_DESCRIPTOR_HEAP_DESC d{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 12,
				D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			if (FAILED(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap)))) { return false; }
		}
		return true;
	}

	bool VulkanNRHost::Initialize(const FeatureSession::CreateInfo& info)
	{
		capability_ = Probe(info.device, true, true);
		if (!capability_.available) { status_ = capability_.reason; return false; }
		if (const auto* reason = runtimeSession.Claim(Backend::Vulkan)) { status_ = reason; return false; }
		device_ = info.device;
		width_ = info.displayWidth;
		height_ = info.displayHeight;
		guideWidth_ = info.renderWidth;
		guideHeight_ = info.renderHeight;
		if (!CreateShaders(info.device)) {
			status_ = "Vulkan NR conversion shaders could not initialize";
			return false;
		}
		// Copy timestamps are in the host queue's domain; inference stays in Vulkan's.
		(void)timestamps_.Initialize(device_.Get(), static_cast<UINT>(vknr::Slots * 4));

		vknr::Create create;
		create.device = info.device;
		const auto weights = Weights();
		const auto directory = Directory();
		wchar_t local[32768]{};
		const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
		if (!length || length >= 32768) {
			status_ = "Vulkan NR cache location is unavailable";
			return false;
		}
		const auto cache = std::filesystem::path(local) / L"TheosRenderPipeline" / L"PipelineCache" / L"VulkanNR";
		std::error_code ec;
		std::filesystem::create_directories(cache, ec);
		if (ec) {
			status_ = "Vulkan NR cache directory could not be created";
			return false;
		}
		create.weightsPath = weights.c_str();
		create.shaderRoot = directory.c_str();
		create.cacheRoot = cache.c_str();

		struct SharedHandles
		{
			std::array<HANDLE, 5> values{};
			~SharedHandles()
			{
				for (auto h : values) {
					if (h) { CloseHandle(h); }
				}
			}
		} handles;

		constexpr DXGI_FORMAT kFormats[4]{
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R32G32_FLOAT,
			DXGI_FORMAT_R32_FLOAT,
			DXGI_FORMAT_R16G16B16A16_FLOAT
		};
		for (unsigned i = 0; i < 4; ++i) {
			const bool guide = (i == 1 || i == 2);
			const UINT w = guide ? guideWidth_ : width_;
			const UINT h = guide ? guideHeight_ : height_;
			if (FAILED(D3D12Helpers::CreateDefaultTexture2D(device_.Get(), w, h, kFormats[i],
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &images_[i], D3D12_HEAP_FLAG_SHARED)) ||
				FAILED(device_->CreateSharedHandle(images_[i].Get(), nullptr, GENERIC_ALL, nullptr, &handles.values[i]))) {
				status_ = "Vulkan NR shared texture creation refused";
				return false;
			}
			create.images[i] = { handles.values[i], w, h, kFormats[i] };
		}
		if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_))) ||
			FAILED(device_->CreateSharedHandle(fence_.Get(), nullptr, GENERIC_ALL, nullptr, &handles.values[4]))) {
			status_ = "Vulkan NR shared fence creation refused";
			return false;
		}
		create.fence = handles.values[4];
#ifdef TRP_NR_VENDOR_BENCHMARK
		create.validation = GetEnvironmentVariableW(L"TRP_VKNR_TEST_VALIDATION", nullptr, 0) > 0;
#endif
		const auto started = std::chrono::steady_clock::now();
		if (g_api.create(&create, &handle_, &capability_) != vknr::Result::Ok) {
			unsafe_ = handle_ != nullptr;
			status_ = g_api.error(handle_);
			return false;
		}
		// Start the watchdog only after engine creation succeeds so a failed init
		// never leaks a watchdog thread on a quarantined host.
		// 10 s without progress on submitted NR work: far beyond Windows' 2 s GPU
		// timeout, so ordinary hitches, loading and slow frames never reach it.
		watchdog_.Start(fence_.Get(), std::chrono::milliseconds(10000));
		status_ = std::format("Vulkan: {} ({})", PathLabel(capability_.path), capability_.revision);
		logger::info("[Vulkan NR] backend=Vulkan revision={} path={} device={} model={}x{} guides={}x{} initialization={:.1f} ms; {}",
			capability_.revision, PathLabel(capability_.path), capability_.device, width_, height_, guideWidth_, guideHeight_,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), capability_.pathReason);
		return true;
	}

	void VulkanNRHost::Dispatch(ID3D12GraphicsCommandList* list, std::size_t slot,
		const FeatureSession::EvaluationInput& input, bool pack)
	{
		const auto query = static_cast<UINT>(slot * 4 + (pack ? 0 : 2));
		if (timestamps_ && queryFrequency_) {
			list->EndQuery(timestamps_.Heap(), D3D12_QUERY_TYPE_TIMESTAMP, query);
		}
		const auto stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto* heap = heaps_[slot].Get();
		auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
		auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
		if (!pack) {
			cpu.ptr += 6 * stride;
			gpu.ptr += 6 * stride;
		}
		ID3D12Resource* sources[]{ pack ? input.color : images_[3].Get(), input.motionVectors, input.depth };
		ID3D12Resource* dests[]{ pack ? images_[0].Get() : input.output, images_[1].Get(), images_[2].Get() };
		const unsigned count = pack ? 3u : 1u;
		D3D12Helpers::BarrierBatch before{ list };
		for (unsigned i = 0; i < count; ++i) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = sources[i]->GetDesc().Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			device_->CreateShaderResourceView(sources[i], &srv, { cpu.ptr + i * stride });

			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = dests[i]->GetDesc().Format;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device_->CreateUnorderedAccessView(dests[i], nullptr, &uav, { cpu.ptr + (i + 3) * stride });

			before.Add(sources[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
				.Add(dests[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
		before.Flush();

		list->SetDescriptorHeaps(1, &heap);
		list->SetComputeRootSignature(root_.Get());
		list->SetPipelineState(pack ? pack_.Get() : unpack_.Get());
		list->SetComputeRootDescriptorTable(0, gpu);
		const UINT sizes[]{ width_, height_, guideWidth_, guideHeight_ };
		list->SetComputeRoot32BitConstants(1, 4, sizes, 0);
		const UINT dispatchW = pack ? (std::max)(width_, guideWidth_) : width_;
		const UINT dispatchH = pack ? (std::max)(height_, guideHeight_) : height_;
		list->Dispatch((dispatchW + 7) / 8, (dispatchH + 7) / 8, 1);

		D3D12Helpers::BarrierBatch after{ list };
		for (unsigned i = 0; i < count; ++i) {
			after.Add(sources[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
				.Add(dests[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		}
		after.Flush();

		if (timestamps_ && queryFrequency_) {
			list->EndQuery(timestamps_.Heap(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
			list->ResolveQueryData(timestamps_.Heap(), D3D12_QUERY_TYPE_TIMESTAMP, query, 2,
				timestamps_.Readback(), query * sizeof(std::uint64_t));
			queryPending_[slot][pack ? 0 : 1] = true;
		}
	}

	bool VulkanNRHost::WatchdogFired()
	{
		if (!watchdog_.Fired()) { return false; }
		if (!unsafe_) {
			logger::error("[Vulkan NR] backend=Vulkan stalled: value {} not signalled within {} ms; "
				"D3D12 queue released, Vulkan NR off for this session",
				watchdog_.FiredValue(), watchdog_.Timeout().count());
		}
		runtimeSession.Fail(Backend::Vulkan, kVulkanStalled);
		unsafe_ = true;
		status_ = kVulkanStalled;
		return true;
	}

	bool VulkanNRHost::BeginFrame(std::size_t slot, ID3D12CommandQueue* queue, bool& ready)
	{
		ready = false;
		if (!handle_ || !queue || slot >= vknr::Slots || WatchdogFired()) { return false; }
		++measurements_.frames;
		if (!queryFrequency_) { queue->GetTimestampFrequency(&queryFrequency_); }
		for (unsigned kind = 0; kind < 2; ++kind) {
			if (!queryPending_[slot][kind]) { continue; }
			const auto i = slot * 4 + kind * 2;
			if (timestamps_ && queryFrequency_ && timestamps_.Values()[i + 1] >= timestamps_.Values()[i]) {
				measurements_.copyTotalMs +=
					static_cast<double>(timestamps_.Values()[i + 1] - timestamps_.Values()[i]) * 1000.0 / queryFrequency_;
			}
			queryPending_[slot][kind] = false;
		}
		vknr::Completion completion;
		if (g_api.poll(handle_, &completion) != vknr::Result::Ok) {
			// A failed poll (for example Vulkan device lost) will never signal: release now.
			const bool released = watchdog_.ReleaseNow();
			unsafe_ = true;
			status_ = g_api.error(handle_);
			if (released) {
				runtimeSession.Fail(Backend::Vulkan, kVulkanStalled);
				logger::error("[Vulkan NR] backend=Vulkan engine failure: {}; held D3D12 queue released, Vulkan NR off for this session",
					status_);
			}
			return false;
		}
		measurements_.completed = completion.samples;
		measurements_.networkTotalMs = completion.totalMilliseconds;
		ready = backgroundReady_ = backgroundPending_ && completion.done >= value_;
		return true;
	}

	bool VulkanNRHost::Consume(const FeatureSession::EvaluationInput& input, ID3D12CommandQueue* queue,
		std::size_t slot, ID3D12GraphicsCommandList* list)
	{
		if (!backgroundReady_ || !queue || !list || WatchdogFired()) { return false; }
		// Completion has already been proved by VkFence. This wait establishes API
		// ownership/visibility, but cannot hold the host queue behind unfinished NR.
		if (FAILED(queue->Wait(fence_.Get(), value_))) {
			unsafe_ = true;
			status_ = "Vulkan completed-result wait failed";
			return false;
		}
		Dispatch(list, slot, input, false);
		backgroundPending_ = backgroundReady_ = false;
		return true;
	}

	bool VulkanNRHost::DropCompleted(ID3D12CommandQueue* queue)
	{
		if (!backgroundReady_ || !queue) { return false; }
		// Reacquire shared-image ownership even when the stale pixels are discarded.
		if (FAILED(queue->Wait(fence_.Get(), value_))) {
			unsafe_ = true;
			status_ = "Vulkan stale-result ownership wait failed";
			return false;
		}
		backgroundPending_ = backgroundReady_ = false;
		return true;
	}

	bool VulkanNRHost::Evaluate(const FeatureSession::EvaluationInput& input, SourceDLSSG::Interop& interop,
		ID3D12CommandQueue* queue, std::size_t slot, ID3D12GraphicsCommandList*& list,
		bool background, bool frameOpened)
	{
		if (!handle_ || !queue || slot >= vknr::Slots) {
			status_ = "Vulkan NR submission context missing";
			return false;
		}
		if (WatchdogFired()) { return false; }
		if (backgroundPending_) {
			status_ = "Vulkan job is still reserved";
			return false;
		}
		if (!background && !frameOpened) {
			bool ready{};
			if (!BeginFrame(slot, queue, ready)) { return false; }
		}
		const auto color = input.color->GetDesc();
		const auto out = input.output->GetDesc();
		const auto mv = input.motionVectors->GetDesc();
		const auto depth = input.depth->GetDesc();
		if (color.Width != width_ || color.Height != height_ || out.Width != width_ || out.Height != height_ ||
			color.Format != out.Format ||
			(color.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && color.Format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
			mv.Width != guideWidth_ || mv.Height != guideHeight_ || depth.Width != guideWidth_ || depth.Height != guideHeight_) {
			status_ = "Vulkan NR input extent/format refused before submission";
			return false;
		}
		vknr::Frame frame;
		frame.slot = static_cast<UINT>(slot);
		frame.ready = value_ + 1;
		frame.done = value_ + 2;
		// The TRP evaluator expresses model-pixel displacement; the Vulkan engine consumes UV displacement.
		frame.motionScaleX = input.motionVectorScaleX / width_;
		frame.motionScaleY = input.motionVectorScaleY / height_;
		frame.localTone = input.tuning.localToneStrength;
		frame.localStructure = input.tuning.localStructureStrength;
		frame.skin = input.tuning.skinStructureStrength;
		frame.autoSkin = input.tuning.useAutoSkinMask;
		frame.style = input.tuning.style;
		frame.reset = input.reset;
		frame.depthInverted = input.depthInverted;

		const auto prepared = g_api.prepare(handle_, &frame);
		if (prepared != vknr::Result::Ok) {
			status_ = prepared == vknr::Result::Busy ? "Vulkan NR slot has not retired" : g_api.error(handle_);
			return false;
		}
		Dispatch(list, slot, input, true); // All touched D3D12 resources return to COMMON.
		if (FAILED(interop.Split(SourceDLSSG::Work::Upscaling, &list))) {
			unsafe_ = true;
			status_ = "Vulkan NR producer submission failed";
			return false;
		}
		if (FAILED(queue->Signal(fence_.Get(), frame.ready))) {
			unsafe_ = true;
			status_ = "Vulkan NR ready signal failed";
			return false;
		}
		if (g_api.submit(handle_) != vknr::Result::Ok) {
			// frame.ready was already signaled on the shared fence; never reuse it.
			value_ = frame.ready;
			unsafe_ = true;
			status_ = g_api.error(handle_);
			return false;
		}
		value_ = frame.done;
		if (background) {
			backgroundPending_ = true;
			++frames_;
			return true;
		}
		// Never queue a wait for an unsubmitted value. Track the wait independently
		// of the consumer list so recovery can discard that list and still retire NR.
		if (FAILED(queue->Wait(fence_.Get(), frame.done))) {
			unsafe_ = true;
			status_ = "Vulkan NR consumer wait could not be queued";
			return false;
		}
		// From here the queue is held until Vulkan, or the watchdog, signals frame.done.
		// If the watchdog already fired on an earlier frame, Arm signals frame.done immediately.
		if (!watchdog_.Arm(frame.done)) {
			WatchdogFired();
			return false;
		}
		if (FAILED(interop.TrackExternal(SourceDLSSG::Work::Upscaling))) {
			unsafe_ = true;
			status_ = "Vulkan NR consumer wait retirement unconfirmed";
			return false;
		}
		Dispatch(list, slot, input, false);
		if ((++frames_ % 120) == 0) {
			logger::info("[Vulkan NR] path={} completed network={:.3f} ms; host handoff/copies included in NR per frame; frames={}",
				PathLabel(capability_.path), g_api.timing(handle_), frames_);
		}
		return true;
	}

	bool VulkanNRHost::ReleaseAfterRetirement()
	{
		if (unsafe_ || WatchdogFired()) {
			// Host D3D12 queue is already drained on retirement; stop the watchdog
			// thread so a quarantined owner does not keep a background thread alive.
			watchdog_.Stop();
			return false;
		}
		if (handle_ && g_api.drain(handle_) != vknr::Result::Ok) {
			unsafe_ = true;
			watchdog_.Stop();
			status_ = "Vulkan shutdown drain unconfirmed; complete owner retained";
			return false;
		}
		if (handle_ && g_api.release(handle_) != vknr::Result::Ok) {
			watchdog_.Stop();
			status_ = "Vulkan NR retirement unconfirmed; complete owner retained";
			return false;
		}
		handle_ = nullptr;
		watchdog_.Stop();
		return true;
	}

	VulkanNRHost::~VulkanNRHost()
	{
		// The FeatureSession deleter retains this entire owner on a refusal.
		assert(!handle_);
	}
}
