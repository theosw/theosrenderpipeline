#include "ReShadeIntegration.h"
#include "SourceDLSSGBackend.h"
#include <PCH.h>
#include "CommunityShaderIntegration.h"
#include "SourceDLSSGSwapChain.h"
#include "../PluginPaths.h"
#include <d3dcompiler.h>

namespace TheosRenderPipeline::SourceDLSSG
{
	namespace
	{
		constexpr std::array<const wchar_t*, 6> kStreamlineModules{
			L"nvngx_dlssg.dll", L"sl.common.dll", L"sl.dlss_g.dll",
			L"sl.interposer.dll", L"sl.pcl.dll", L"sl.reflex.dll"
		};
	}
	using Microsoft::WRL::ComPtr;
	Backend& Backend::Get()
	{
		// Streamline owns background work and DXGI proxies. Keep its module and
		// devices alive until process exit; no uncertain DLL-unload ordering.
		static auto* backend = new Backend;
		return *backend;
	}
	bool Backend::Check(HRESULT a_result, const char* a_operation)
	{
		if (SUCCEEDED(a_result)) { return true; }
		if (SUCCEEDED(fault_)) {
			fault_ = a_result;
			status_ = std::format("{} failed HRESULT=0x{:08X}", a_operation, static_cast<std::uint32_t>(a_result));
			logger::error("[SourceDLSSG] {}", status_);
		}
		return false;
	}
	bool Backend::Check(sl::Result a_result, const char* a_operation)
	{
		if (a_result == sl::Result::eOk) { return true; }
		const auto detail = std::format("{} Streamline result={}", a_operation, static_cast<std::uint32_t>(a_result));
		return Check(E_FAIL, detail.c_str());
	}
	bool Backend::CheckSession(bool a_result)
	{
		const auto& s = session_.Snapshot();
		if (a_result) {
			if (s.stateQueryResult != reportedStateQueryResult_) {
				if (s.stateQueryResult == sl::Result::eWarnOutOfVRAM) {
					logger::warn("[SourceDLSSG] DLSS-G state eWarnOutOfVRAM frame={} warnings={}; VRAM budget warning; frame generation settings unchanged",
						s.frameIndex, s.stateWarnings);
				}
				reportedStateQueryResult_ = s.stateQueryResult;
			}
			return true;
		}
		const auto detail = std::format("session {} failure={} result={} frame={}",
			s.operation, static_cast<int>(s.failure), static_cast<int>(s.result), s.frameIndex);
		return Check(E_FAIL, detail.c_str());
	}
	bool Backend::Load(const std::filesystem::path& a_directory)
	{
		using namespace TheosRenderPipeline::PluginPaths;
		if (!a_directory.is_absolute()) {
			return Check(E_INVALIDARG, "Streamline directory must be absolute");
		}
		if (::GetModuleHandleW(L"PDPerfPlugin.dll") || ::GetModuleHandleW(L"SkyrimUpscaler.dll")) {
			return Check(E_UNEXPECTED, "another frame-generation mod is already loaded; disable the competing mod");
		}
		directory_ = Normalize(a_directory);
		// Keep one owner for the configured modules. Versions do not gate loading.
		for (const auto* name : kStreamlineModules) {
			if (HasConflictingLoadedModule(directory_ / name, CommunityShaders::Active())) {
				return Check(E_UNEXPECTED, std::format("{} already loaded by another owner", std::filesystem::path(name).string()).c_str());
			}
		}
		mfgUnlock_.BeforeStreamline(device12_.Get(), directory_);
		interposer_ = ::LoadLibraryExW((directory_ / L"sl.interposer.dll").c_str(), nullptr,
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (!interposer_) { return Check(HRESULT_FROM_WIN32(::GetLastError()), "load interposer"); }
		auto load = [&](auto& function, const char* name) {
			function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(::GetProcAddress(interposer_, name));
			return function != nullptr || Check(E_NOINTERFACE, name);
		};
		if (!load(init_, "slInit") || !load(loadFeature_, "slSetFeatureLoaded") ||
			!load(setDevice_, "slSetD3DDevice") || !load(supported_, "slIsFeatureSupported") ||
			!load(featureFunction_, "slGetFeatureFunction") || !load(upgrade_, "slUpgradeInterface") ||
			!load(api_.newFrameToken, "slGetNewFrameToken") || !load(api_.setConstants, "slSetConstants") ||
			!load(api_.setTag, "slSetTag")) { return false; }
		const wchar_t* paths[]{ directory_.c_str() };
		const sl::Feature features[]{ sl::kFeatureReflex, sl::kFeatureDLSS_G };
		sl::Preferences preferences{};
		preferences.pathsToPlugins = paths;
		preferences.numPathsToPlugins = 1;
		preferences.featuresToLoad = features;
		preferences.numFeaturesToLoad = 2;
		preferences.renderAPI = sl::RenderAPI::eD3D12;
		preferences.engine = sl::EngineType::eCustom;
		preferences.engineVersion = "TheosRenderPipeline 0.1";
		// Share the project identity with the D3D11 DLSS backend.
		preferences.projectId = "f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11";
		preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
			sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseDXGIFactoryProxy;
		preferences.logLevel = sl::LogLevel::eDefault;
		preferences.logMessageCallback = StreamlineLogCallback;
		if (!Check(init_(preferences, sl::kSDKVersion), "slInit")) { return false; }
		for (const auto feature : features) {
			if (!Check(loadFeature_(feature, true), "slSetFeatureLoaded")) { return false; }
		}
		logger::info("[SourceDLSSG] Streamline initialized from {} SDK=2.11.1 manual factory proxy; OTA disabled", directory_.string());
		return true;
	}
	void Backend::StreamlineLogCallback(sl::LogType a_type, const char* a_message)
	{
		const std::string_view message = a_message ? a_message : "";
		Backend::Get().runtimeDiagnostics_.Record(message);
		switch (a_type) {
		case sl::LogType::eError:
			logger::error("[SourceDLSSG/SL {}] {}", static_cast<int>(a_type), message); break;
		case sl::LogType::eWarn:
			logger::warn("[SourceDLSSG/SL {}] {}", static_cast<int>(a_type), message); break;
		default:
			logger::info("[SourceDLSSG/SL {}] {}", static_cast<int>(a_type), message); break;
		}
	}
	HRESULT Backend::CreateSwapChain(IDXGIFactory* a_factory, ID3D11Device* a_device,
		const DXGI_SWAP_CHAIN_DESC& a_desc, const std::filesystem::path& a_directory, IDXGISwapChain** a_result)
	{
		if (!a_result) { return E_POINTER; }
		*a_result = nullptr;
		if (attempted_ || !a_factory || !a_device || !a_desc.OutputWindow ||
			a_desc.SampleDesc.Count != 1 || a_desc.SampleDesc.Quality != 0) { return E_INVALIDARG; }
		attempted_ = true;
		// No fake HDR reinterpretation. The native transport supports equal-format
		// copies; unsupported swapchain formats fail before Streamline is loaded.
		if (a_desc.BufferDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
			a_desc.BufferDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
			a_desc.BufferDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
			a_desc.BufferDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) { return DXGI_ERROR_UNSUPPORTED; }
		ComPtr<IDXGIFactory5> capabilities;
		BOOL tearing = FALSE;
		if (FAILED(a_factory->QueryInterface(IID_PPV_ARGS(&capabilities))) ||
			FAILED(capabilities->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) || !tearing) {
			Check(DXGI_ERROR_UNSUPPORTED, "Streamline immediate presentation requires tearing support");
			return fault_;
		}
		device11_ = a_device;
		device11_->GetImmediateContext(&context11_);
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> adapter;
		if (!Check(device11_.As(&dxgiDevice), "D3D11 DXGI device") ||
			!Check(dxgiDevice->GetAdapter(&adapter), "D3D11 adapter") ||
			!Check(ReShadeIntegration::Get().CreateSourceDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, &device12_), "D3D12 device")) { return fault_; }
		logger::info("[ReShade] {}", ReShadeIntegration::Get().Status());
		MFGUnlock::StartupScope startupScope(mfgUnlock_);
		if (!Load(a_directory)) { return fault_; }
		void* upgraded = device12_.Get();
		mfgUnlock_.Prepare(device12_.Get(), directory_);
		device12_->AddRef();
		const auto upgradedResult = upgrade_(&upgraded);
		upgradedDevice12_.Attach(static_cast<ID3D12Device*>(upgraded));
		if (!Check(upgradedResult, "upgrade D3D12 device") || !upgradedDevice12_ ||
			!Check(setDevice_(device12_.Get()), "slSetD3DDevice")) { return E_FAIL; }
		auto luid = device12_->GetAdapterLuid();
		sl::AdapterInfo info{};
		info.deviceLUID = reinterpret_cast<std::uint8_t*>(&luid);
		info.deviceLUIDSizeInBytes = sizeof(luid);
		for (const auto feature : { sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G }) {
			if (!Check(supported_(feature, info), std::format("feature support {}", feature).c_str())) { return fault_; }
		}
		for (std::size_t index = 0; index < kStreamlineModules.size(); ++index) {
			const auto* name = kStreamlineModules[index];
			runtimeModules_[index] = TheosRenderPipeline::PluginPaths::RetainLoadedModule(directory_ / name);
			if (!runtimeModules_[index]) {
				Check(E_FAIL, std::format("{} was not loaded from the configured Streamline directory", std::filesystem::path(name).string()).c_str());
				return fault_;
			}
			logger::info("[SourceDLSSG] loaded {}", (directory_ / name).string());
		}
		auto resolve = [&](sl::Feature feature, const char* name, auto& function) {
			void* address = nullptr;
			if (!Check(featureFunction_(feature, name, address), name) || !address) { return Check(E_NOINTERFACE, name); }
			function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
			return true;
		};
		if (!resolve(sl::kFeatureReflex, "slReflexSetOptions", api_.setReflexOptions) ||
			!resolve(sl::kFeatureReflex, "slReflexSleep", api_.reflexSleep) ||
			!resolve(sl::kFeatureReflex, "slReflexGetState", api_.getReflexState) ||
			!resolve(sl::kFeaturePCL, "slPCLSetMarker", api_.marker) ||
			!resolve(sl::kFeatureDLSS_G, "slDLSSGGetState", api_.getState) ||
			!resolve(sl::kFeatureDLSS_G, "slDLSSGSetOptions", api_.setOptions)) { return fault_; }
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		mfgUnlock_.BindWrapper(reinterpret_cast<const void*>(api_.setOptions));
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (!Check(device12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), "presenting queue") ||
			!Check(interop_.Initialize(device11_.Get(), device12_.Get(), queue_.Get()), "shared interop")) { return fault_; }
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (FAILED(queue_->GetTimestampFrequency(&neuralTimestampFrequency_)) || !neuralTimestampFrequency_) {
			neuralTimestampFrequency_ = 0;
			logger::warn("[DLSSNR Source] presenting queue does not expose GPU timestamp frequency; CPU boundary timing remains available");
		}
#endif
		void* factory = a_factory;
		a_factory->AddRef();
		const auto factoryResult = upgrade_(&factory);
		ComPtr<IDXGIFactory> upgradedFactory;
		upgradedFactory.Attach(static_cast<IDXGIFactory*>(factory));
		if (!Check(factoryResult, "upgrade factory") || !upgradedFactory) { return E_FAIL; }
		auto desc = a_desc;
		desc.BufferDesc.Format = PresentationFormat(a_desc.BufferDesc.Format);
		desc.BufferCount = 2;
		desc.Flags &= ~(0x2000u | 0x4000u);
		// The pinned DLSS-G runtime submits immediate Presents with ALLOW_TEARING,
		// including generation-off startup. DXGI requires this creation flag.
		desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		ComPtr<IDXGISwapChain> native;
		if (!Check(upgradedFactory->CreateSwapChain(queue_.Get(), &desc, &native), "Streamline swapchain")) { return fault_; }
		retainedNative_ = native;
		api_.context = this;
		api_.waitForInputReaders = [](void* context, void* fence, std::uint64_t value) {
			auto& owner = *static_cast<Backend*>(context);
			const auto result = owner.interop_.WaitForInputReaders(static_cast<ID3D12Fence*>(fence), value);
			if (FAILED(result)) {
				const auto& detail = owner.interop_.LastInputWait();
				logger::error("[SourceDLSSG] input reuse fence stage={} fence={} value={} hostIdentity={} referenceFenceOwner={} inputFenceOwner={}",
					detail.stage, fence, value, detail.hostIdentity, detail.referenceFenceOwner, detail.inputFenceOwner);
			}
			return owner.Check(result, "input reuse fence");
		};
		session_.RequestReflexMode(ReflexConfiguration());
		session_.RequestOutputFPSLimit(outputFPSLimit_.load(std::memory_order_relaxed));
		if (!CheckSession(session_.Start(api_, 1))) { return fault_; }
		mfgUnlock_.Tick();
		session_.SetMFGUnlockState(mfgUnlock_.Snapshot().UsesCompatibilityUnlock(), mfgUnlock_.Snapshot().Ready());
		session_.RequestGeneration(GenerationConfiguration());
		auto* wrapper = new (std::nothrow) SwapChain(native.Get(), *this, a_desc.BufferDesc.Format);
		if (!wrapper) { return E_OUTOFMEMORY; }
		const auto rebuild = wrapper->RebuildBuffers();
		if (FAILED(rebuild)) { wrapper->Release(); return rebuild; }
		ready_ = true;
		status_ = "source Streamline swapchain ready; generation off until valid inputs";
		logger::info("[SourceDLSSG] {} {}x{} format={} buffers=2", status_, desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format));
		*a_result = wrapper;
		return S_OK;
	}
	void Backend::ReleaseGuides()
	{
		motion_ = {}; depth_ = {}; ui_ = {}; hudless_ = {}; earlyNeuralColor_ = {};
		uiSource_.Reset(); depthCopy_.ResetViews();
	}
	bool Backend::EnsureGuide(ID3D11Texture2D* a_source, SharedTexture& a_pair, DXGI_FORMAT a_format, FrameExtent a_extent)
	{
		if (!a_source) { return false; }
		D3D11_TEXTURE2D_DESC desc{};
		a_source->GetDesc(&desc);
		if (!a_extent.width && !a_extent.height) { a_extent = {desc.Width, desc.Height}; }
		if (!a_extent.Fits(desc)) { return false; }
		desc.Width = a_extent.width; desc.Height = a_extent.height;
		if (a_format != DXGI_FORMAT_UNKNOWN) { desc.Format = a_format; }
		if (a_pair.texture11 && (a_pair.desc.Width != desc.Width || a_pair.desc.Height != desc.Height || a_pair.desc.Format != desc.Format)) {
			if (!Check(interop_.SignalD3D11(Work::FrameGeneration), "retire changed guide") ||
				!Check(interop_.Drain(), "drain changed guide")) { return false; }
			a_pair = {};
			if (&a_pair == &depth_) { depthCopy_.ResetViews(); }
		}
		if (a_pair.texture11) { return true; }
		desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (&a_pair == &depth_) { desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS; }
		const auto allocation = interop_.CreateSharedTexture(desc, a_pair);
		if (FAILED(allocation)) {
			logger::error("[SourceDLSSG] shared guide format={} size={}x{} bind=0x{:X} mip={} array={}",
				static_cast<int>(desc.Format), desc.Width, desc.Height, desc.BindFlags, desc.MipLevels, desc.ArraySize);
		}
		return Check(allocation, "shared guide allocation");
	}
	bool Backend::CopyDepth(ID3D11Texture2D* a_depth)
	{
		return Check(depthCopy_.Copy(context11_.Get(), a_depth, depth_.texture11.Get(),
			{depth_.desc.Width, depth_.desc.Height}), "depth copy");
	}
	bool Backend::Prepare(const sl::Constants& a_constants, ID3D11Texture2D* a_motion, ID3D11Texture2D* a_depth,
		ID3D11Texture2D* a_ui, ID3D11Texture2D* a_hudless, FrameExtent a_renderExtent,
		UINT a_width, UINT a_height, bool a_neuralEligible)
	{
		if (!Ready() || !Session::ValidConstants(a_constants) || !a_motion || !a_depth ||
			!a_renderExtent.width || !a_renderExtent.height) { return false; }
		if (neuralEvaluatedEarly_) {
			// Early NR froze these before the producer could overwrite its depth.
			// Never reuse them for a differently sized completed frame.
			if (motion_.desc.Width != a_renderExtent.width || motion_.desc.Height != a_renderExtent.height ||
				depth_.desc.Width != a_renderExtent.width || depth_.desc.Height != a_renderExtent.height) { return false; }
		} else if (!EnsureGuide(a_motion, motion_, DXGI_FORMAT_UNKNOWN, a_renderExtent) ||
			!EnsureGuide(a_depth, depth_, DXGI_FORMAT_R32_FLOAT, a_renderExtent) ||
			!CopyDepth(a_depth) || !Check(interop_.CopyInputRegion(a_motion, motion_, a_renderExtent), "motion copy")) { return false; }
		if (a_hudless && (!EnsureGuide(a_hudless, hudless_) || !Check(interop_.CopyInput(a_hudless, hudless_), "HUD-less copy"))) { return false; }
		if (a_ui && !EnsureGuide(a_ui, ui_)) { return false; }
		uiSource_ = a_ui;
		frameConstants_ = a_constants;
		const bool eligible = a_neuralEligible && !TransitionBlocked() && a_hudless &&
			(a_ui || (neuralEvaluatedEarly_ && frameNeuralOptions_.worldOnly));
		// The game host freezes settings before DLSS. Standalone callers that
		// only Prepare retain the late-stage contract and never run early NR here.
		if (!neuralFrameBegun_) {
			frameNeuralOptions_ = NeuralConfiguration();
			neuralEligible_ = eligible && !frameNeuralOptions_.WorldOnly();
			frameNeuralReset_ = neuralHistory_.ResetFor(frameNeuralOptions_, neuralEligible_, a_constants.reset == sl::eTrue);
		} else {
			neuralEligible_ = neuralEligible_ && eligible;
			frameNeuralReset_ |= a_constants.reset == sl::eTrue;
			if (!neuralEligible_) { neuralHistory_.Invalidate(); }
		}
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (frameNeuralOptions_.enabled && neuralEligible_ && neuralPass_ &&
			neuralPass_->NeedsRecreation(frameNeuralOptions_, motion_.desc.Width, motion_.desc.Height)) { frameNeuralReset_ = true; }
#endif
		if (frameNeuralReset_) { frameConstants_.reset = sl::eTrue; }
		auto tag = [](const SharedTexture& pair) {
			TaggedTexture t;
			t.resource = sl::Resource(sl::ResourceType::eTex2d, pair.texture12.Get(), D3D12_RESOURCE_STATE_COMMON);
			t.resource.width = pair.desc.Width; t.resource.height = pair.desc.Height;
			t.extent = { 0, 0, pair.desc.Width, pair.desc.Height };
			return t;
		};
		FrameGuides guides;
		guides.motion = tag(motion_); guides.depth = tag(depth_);
		if (a_ui) { guides.ui = tag(ui_); }
		if (a_hudless) { guides.hudless = tag(hudless_); }
		guides.displayWidth = a_width; guides.displayHeight = a_height;
		if (!Check(interop_.SignalD3D11(Work::FrameGeneration), "D3D11 guides ready")) { return false; }
		ID3D12GraphicsCommandList* list = nullptr;
		if (!Check(interop_.Begin(Work::FrameGeneration, &list), "begin guide tags") ||
			!CheckSession(session_.Prepare(frameConstants_, guides, list)) ||
			!Check(interop_.Submit(Work::FrameGeneration), "submit guide tags")) { return false; }
		return true;
	}
	HRESULT Backend::BeforePresent(ID3D12Resource* a_source, ID3D12Resource* a_destination)
	{
		if (!Ready()) { return FAILED(fault_) ? fault_ : E_UNEXPECTED; }
		const auto oldReflexRequest = session_.Snapshot().reflexRequested;
		const auto oldReflexSubmitted = session_.Snapshot().reflexSubmitted;
		const auto oldFrameLimit = session_.Snapshot().frameLimitSubmittedUs;
		const auto oldGeneration = session_.Snapshot().generationRequested;
		const auto oldLimited = session_.Snapshot().generationLimited;
		mfgUnlock_.Tick();
		session_.SetMFGUnlockState(mfgUnlock_.Snapshot().UsesCompatibilityUnlock(), mfgUnlock_.Snapshot().Ready());
		session_.RequestGeneration(GenerationConfiguration());
		session_.RequestReflexMode(ReflexConfiguration());
		session_.RequestOutputFPSLimit(outputFPSLimit_.load(std::memory_order_relaxed));
		const bool prepared = session_.Snapshot().stage == SessionStage::Rendering;
		if (prepared) {
			if (uiSource_ && !Check(interop_.CopyInput(uiSource_.Get(), ui_), "late UI copy")) { return fault_; }
		}
#if !defined(TRP_NO_NEURAL_RENDERING)
		const auto options = prepared ? frameNeuralOptions_ : NeuralConfiguration();
		const auto unavailable = NeuralUnavailableReason(options);
		const bool eligible = prepared && neuralEligible_ && !unavailable;
		const bool active = options.enabled && eligible && (!options.WorldOnly() || neuralEvaluatedEarly_);
		const bool lateActive = active && !options.WorldOnly();
		const bool reset = prepared ? frameNeuralReset_ : neuralHistory_.ResetFor(options, false, false);
		if (lateActive && !RecreateNeuralIfNeeded(options)) { return fault_; }
#endif
		ID3D12GraphicsCommandList* list = nullptr;
		if (!Check(interop_.SignalD3D11(Work::SwapChain), "native D3D11 output ready") ||
			!Check(interop_.Begin(Work::SwapChain, &list), "begin native output copy")) { return fault_; }
		auto* realSource = a_source;
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (lateActive) {
			if (!neuralPass_) { neuralPass_ = std::make_unique<NeuralPass>(); }
			if (!neuralPass_->Record(device12_.Get(), list, interop_.CurrentSlot(Work::SwapChain), options,
				reset, frameConstants_.depthInverted == sl::eTrue,
				frameConstants_.mvecScale.x * motion_.desc.Width, frameConstants_.mvecScale.y * motion_.desc.Height,
				motion_.texture12.Get(), depth_.texture12.Get(), ui_.texture12.Get(), hudless_.texture12.Get(), a_source,
				neuralTimestampFrequency_)) {
				FailNeuralRecording();
				return fault_; // Retain all objects and the unsubmitted list on unknown NGX failure.
			}
			realSource = neuralPass_->Composed();
		}
#endif
		HRESULT outputResult;
		if (realSource->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT && a_destination->GetDesc().Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
			if (!hdrPass_) { hdrPass_ = std::make_unique<HDRPass>(); }
			outputResult = hdrPass_->Record(device12_.Get(), list, interop_.CurrentSlot(Work::SwapChain), realSource, a_destination);
		} else {
			outputResult = Interop::RecordCopy(list, realSource, a_destination);
		}
		const bool transitionBlocked = TransitionBlocked();
		const auto transitionWarmup = transitionWarmupPresents_.load(std::memory_order_acquire);
		const bool generationAllowed = enabled_ && !transitionBlocked && transitionWarmup == 0;
		if (!Check(outputResult, "record native output copy/conversion") ||
			!Check(interop_.Submit(Work::SwapChain), "submit native output copy") ||
			(prepared && !CheckSession(session_.CompleteInputWrites())) ||
			!CheckSession(session_.BeforePresent(generationAllowed))) { return fault_; }
		if (oldReflexRequest != session_.Snapshot().reflexRequested ||
			oldReflexSubmitted != session_.Snapshot().reflexSubmitted || oldFrameLimit != session_.Snapshot().frameLimitSubmittedUs) {
			logger::info("[SourceDLSSG] Reflex frame={} requested={} submitted={} generation={} outputLimitUs={}",
				session_.Snapshot().frameIndex, ReflexModeName(session_.Snapshot().reflexRequested),
				ReflexModeName(session_.Snapshot().reflexSubmitted),
				GenerationEnabled(session_.Snapshot().options.mode), session_.Snapshot().frameLimitSubmittedUs);
		}
		if (oldGeneration != session_.Snapshot().generationRequested || oldLimited != session_.Snapshot().generationLimited) {
			const auto& s = session_.Snapshot();
			logger::info("[SourceDLSSG] MFG requestedGenerated={} requestedDynamic={} targetFPS={} submittedGenerated={} submittedMode={} maxGenerated={} limited={}",
				s.generationRequested.generatedFrames, s.generationRequested.dynamic, s.generationRequested.dynamicTargetFPS,
				s.options.numFramesToGenerate, static_cast<int>(s.options.mode), s.state.numFramesToGenerateMax, s.generationLimited);
		}
#if !defined(TRP_NO_NEURAL_RENDERING)
		{
			std::scoped_lock lock(neuralMutex_);
			const bool changed = neuralSnapshot_.active != active;
			neuralSnapshot_.active = active;
			if (active) { ++neuralSnapshot_.evaluations; }
			if (reset) { ++neuralSnapshot_.resets; }
			if (active) { neuralSnapshot_.telemetry = neuralPass_->Telemetry(); }
			neuralSnapshot_.status = unavailable ? unavailable : active ? neuralPass_->Status() : options.enabled ?
				"NR waiting for world inputs and dedicated native UI; standard DLSS active" : "standard DLSS; source NR is off";
			if (changed || (active && (reset || neuralSnapshot_.evaluations % 600 == 0))) {
				logger::info("[SourceDLSSG NR] frame={} active={} evaluations={} reset={} style={} fgRequested={} {}",
					session_.Snapshot().frameIndex, active, neuralSnapshot_.evaluations, reset,
					options.tuning.style, enabled_, neuralSnapshot_.status);
			}
		}
		neuralFrameBegun_ = neuralEvaluatedEarly_ = false;
#endif
		return S_OK;
	}
	HRESULT Backend::AfterPresent(HRESULT a_result)
	{
		// Preserve DXGI's actual failure before the session records its generic
		// Present fault. Logs and subsequent calls must not replace device loss
		// (or another native error) with an unexplained E_FAIL.
		if (FAILED(a_result)) { Check(a_result, "native Present"); }
		if (!CheckSession(session_.AfterPresent(SUCCEEDED(a_result)))) { return FAILED(a_result) ? a_result : fault_; }
		if (SUCCEEDED(a_result) && !TransitionBlocked()) {
			auto remaining = transitionWarmupPresents_.load(std::memory_order_acquire);
			while (remaining && !transitionWarmupPresents_.compare_exchange_weak(
				remaining, remaining - 1, std::memory_order_acq_rel)) {}
			if (remaining == 1) {
				logger::info("[SourceDLSSG] transition warm-up complete; generation may resume on the next frame");
			}
		}
		const auto& state = session_.Snapshot();
		if (state.frameIndex <= 3 || state.frameIndex % 600 == 0) {
			const auto feedback = PresentationFeedback();
			const auto reflex = session_.ReflexTelemetry();
			const auto batches = session_.OutputBatches();
			const auto runtime = RuntimeDiagnosticState();
			const auto neural = NeuralState();
			logger::info("[SourceDLSSG] frame={} enabled={} status={} presented={} maximum={} inputFence={} value={} runtimePresentedTotal={} stateQueries={} stateQueryResult={} stateWarnings={} presentationEpoch={} nativePresentSamples={} nativeOutputs={} nativeBatchedAdditional={} nativeQueryFailures={} outputBatches={} generatedPresented={} interpolatedNotPresented={} batchMismatches={} unknownOutputBatches={} configurationChanges={} reflexQueries={} reflexFailures={} reflexReports={} reflexAvailable={} latencyReportAvailable={} pacerSkips={} presentSkips={} drops={} identityMismatches={} warmupResets={} syncFailures={} nrGpuSamples={} nrGpuFailures={} nrGpuAvgUs={:.2f} nrGpuMaxUs={:.2f} nrCpuSamples={} nrCpuAvgUs={:.2f} nrCpuMaxUs={:.2f}",
				state.frameIndex, state.GenerationActive(), static_cast<int>(state.state.status),
				state.state.numFramesActuallyPresented, state.state.numFramesToGenerateMax,
				state.state.inputsProcessingCompletionFence, state.state.lastPresentInputsProcessingCompletionFenceValue,
				state.runtimePresentedFrames, state.stateQueries, static_cast<int>(state.stateQueryResult), state.stateWarnings, state.presentationEpoch,
				feedback.samples, feedback.observedOutputs, feedback.batchedAdditionalOutputs, feedback.queryFailures,
				batches.validBatches, batches.generatedOutputs, batches.interpolatedNotPresented,
				batches.invalidBatches, batches.unknownBatches, batches.configurationChanges,
				reflex.queries, reflex.queryFailures, reflex.acceptedReports,
				reflex.lowLatencyAvailable, reflex.latencyReportAvailable,
				runtime.Count(RuntimeDiagnosticEvent::PacerSkip), runtime.Count(RuntimeDiagnosticEvent::PresentSkip),
				runtime.Count(RuntimeDiagnosticEvent::DroppedOutput), runtime.Count(RuntimeDiagnosticEvent::IdentityMismatch),
				runtime.Count(RuntimeDiagnosticEvent::WarmupReset), runtime.Count(RuntimeDiagnosticEvent::SynchronizationFailure),
				neural.telemetry.gpuSamples, neural.telemetry.gpuQueryFailures,
				neural.telemetry.AverageGPUMicroseconds(), neural.telemetry.MaximumGPUMicroseconds(),
				neural.telemetry.cpuSamples, neural.telemetry.AverageCPURecordMicroseconds(),
				neural.telemetry.MaximumCPURecordMicroseconds());
		}
		return a_result;
	}

	PresentationFeedbackSnapshot Backend::PresentationFeedback() const
	{
		std::scoped_lock lock(presentationFeedbackMutex_);
		auto snapshot = presentationFeedback_.Snapshot();
		snapshot.queryFailures = presentationQueryFailures_;
		snapshot.qpcFrequency = presentationQpcFrequency_;
		return snapshot;
	}

	void Backend::RecordPresentationFeedback(std::uint32_t a_presentCount,
		std::int64_t a_observedQpc, std::int64_t a_syncQpc)
	{
		std::scoped_lock lock(presentationFeedbackMutex_);
		if (a_observedQpc <= 0) {
			++presentationQueryFailures_;
			return;
		}
		if (!presentationQpcFrequency_) {
			LARGE_INTEGER frequency{};
			if (QueryPerformanceFrequency(&frequency)) {
				presentationQpcFrequency_ = frequency.QuadPart;
			}
		}
		const auto outcome = presentationFeedback_.Sample(a_presentCount, a_observedQpc, a_syncQpc);
		if (outcome == PresentationSampleOutcome::Baseline) {
			logger::info("[SourceDLSSG PresentFeedback] baseline nativeCount={} observedQpc={} syncQpc={} frequency={}",
				a_presentCount, a_observedQpc, a_syncQpc, presentationQpcFrequency_);
		} else if (outcome == PresentationSampleOutcome::Discontinuity) {
			logger::warn("[SourceDLSSG PresentFeedback] counter or QPC discontinuity; correlation restarted at epoch {}",
				presentationFeedback_.Epoch());
		}
	}

	void Backend::ResetPresentationFeedback()
	{
		std::scoped_lock lock(presentationFeedbackMutex_);
		presentationFeedback_.BeginEpoch();
	}

	void Backend::SetTransitionBlocked(bool a_blocked)
	{
		const auto previous = transitionBlocked_.exchange(a_blocked, std::memory_order_acq_rel);
		if (previous == a_blocked) { return; }
		transitionWarmupPresents_.store(
			a_blocked ? 0 : TransitionWarmupPresents, std::memory_order_release);
		if (a_blocked) {
			logger::info("[SourceDLSSG] transition gate closed; scene processing and generated presentation are suspended");
		} else {
			logger::info("[SourceDLSSG] transition gate opened; waiting for three real presents before generation resumes");
		}
	}
	bool Backend::Quiesce()
	{
		enabled_ = false;
		ResetPresentationFeedback();
		if (FAILED(fault_)) { return false; } // Never destroy a failed, unsubmitted NR recording.
		if (!ready_) { return SUCCEEDED(interop_.Drain()); }
		if (session_.Snapshot().stage != SessionStage::Stopped && !CheckSession(session_.Stop())) { return false; }
		if (!Check(interop_.SignalD3D11(Work::SwapChain), "last D3D11 work before resize") ||
			!Check(interop_.Drain(), "retire before resize")) { return false; }
		ReleaseGuides();
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (neuralPass_) {
			neuralPass_->RetireTelemetry();
			std::scoped_lock lock(neuralMutex_);
			neuralSnapshot_.telemetry = neuralPass_->Telemetry();
		}
		neuralPass_.reset(); // The presenting queue and NVIDIA input readers are retired.
#endif
		hdrPass_.reset();
		neuralHistory_.Invalidate();
		neuralEligible_ = false;
		neuralFrameBegun_ = neuralEvaluatedEarly_ = false;
		{
			std::scoped_lock lock(neuralMutex_);
			neuralSnapshot_.active = false;
			neuralSnapshot_.status = "NR retired for resize; next eligible frame recreates the feature";
		}
		return true;
	}
	bool Backend::ResumeAfterResize()
	{
		return Ready() && CheckSession(session_.ResumeAfterResize());
	}
}
