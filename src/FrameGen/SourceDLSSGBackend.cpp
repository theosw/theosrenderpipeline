#include "ReShadeIntegration.h"
#include "SourceDLSSGBackend.h"
#include <PCH.h>
#include "CommunityShaderIntegration.h"
#include "SourceDLSSGSwapChain.h"
#include "SourceRuntimeModuleDiagnostic.h"
#include "../PluginPaths.h"
#include "../ScreenshotFile.h"
#include "../RenderPipeline.h"
#include "SourceFrameGeneration.h"
#include <d3dcompiler.h>
#include <chrono>
#if !defined(TRP_NO_NEURAL_RENDERING)
#include "NeuralRenderingFeatureSession.h"
#endif

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
		// Interop Begin/Drain results are checked immediately, so a pending
		// extended wait belongs to this operation. A crawling fence is a slow
		// GPU; a frozen one that times out suggests a stalled dependency.
		const auto& failure = interop_.LastFailure();
		RetirementWaitDiagnostics wait;
		const bool extendedWait = interop_.TakeExtendedWait(wait);
		const auto routing = RouteCheck(a_result, fault_, failure, extendedWait ? &wait : nullptr);
		if (routing.logExtendedWait) {
			logger::warn("[SourceDLSSG] {} waited {} ms ({} slices) for {} retirement target={} completed={}->{} result=0x{:08X}",
				a_operation, wait.elapsedMs, wait.slices, WorkName(wait.work),
				wait.target, wait.completedAtStart, wait.completedAtEnd, static_cast<std::uint32_t>(wait.result));
		}
		if (SUCCEEDED(a_result)) { return true; }
		if (routing.latch) {
			fault_ = a_result;
			status_ = std::format("{} failed HRESULT=0x{:08X}", a_operation, static_cast<std::uint32_t>(a_result));
			logger::error("[SourceDLSSG] {}", status_);
			deviceLoss_.Report(a_result, a_operation, FrameIndex(),
				device11_.Get(), device12_.Get(), failure);
		}
		return false;
	}
	bool Backend::Check(sl::Result a_result, const char* a_operation)
	{
		if (a_result == sl::Result::eOk) { return true; }
		const auto detail = std::format("{} Streamline result={}", a_operation, static_cast<std::uint32_t>(a_result));
		return Check(E_FAIL, detail.c_str());
	}
	bool Backend::CheckXeFG(HRESULT result)
	{
		// Read the diagnostic only after the SDK operation has finished. Taking
		// c_str() in another argument can precede a mutation of that string.
		return Check(result, xefg_.Status().c_str());
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
			if (s.optionsResult != reportedOptionsResult_) {
				if (s.optionsResult == sl::Result::eWarnOutOfVRAM) {
					logger::warn("[SourceDLSSG] DLSS-G options eWarnOutOfVRAM frame={} warnings={}; options applied; continuing presentation",
						s.frameIndex, s.optionsWarnings);
				} else if (reportedOptionsResult_ == sl::Result::eWarnOutOfVRAM && s.optionsResult == sl::Result::eOk) {
					logger::info("[SourceDLSSG] DLSS-G options budget warning cleared frame={} warnings={}", s.frameIndex, s.optionsWarnings);
				}
				reportedOptionsResult_ = s.optionsResult;
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
		logger::info("[SourceDLSSG] startup stage=module-ownership directory={} communityShaders={}", directory_.string(), CommunityShaders::Active());
		// Keep one owner for the configured modules. Versions do not gate loading.
		for (const auto* name : kStreamlineModules) {
			if (HasConflictingLoadedModule(directory_ / name, CommunityShaders::Active())) {
				return Check(E_UNEXPECTED, std::format("{} already loaded by another owner", std::filesystem::path(name).string()).c_str());
			}
		}
		mfgUnlock_.BeforeStreamline(device12_.Get(), directory_);
		logger::info("[SourceDLSSG] startup stage=load-interposer");
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
		logger::info("[SourceDLSSG] startup stage=slInit");
		if (!Check(init_(preferences, sl::kSDKVersion), "slInit")) { return false; }
		for (const auto feature : features) {
			const auto operation = std::format("slSetFeatureLoaded feature={}", feature);
			logger::info("[SourceDLSSG] startup stage={}", operation);
			if (!Check(loadFeature_(feature, true), operation.c_str())) { return false; }
		}
		logger::info("[SourceDLSSG] Streamline initialized from {} SDK=2.11.1 manual factory proxy; OTA disabled", directory_.string());
		return true;
	}
	void Backend::StreamlineLogCallback(sl::LogType a_type, const char* a_message)
	{
		const std::string_view message = a_message ? a_message : "";
		Backend::Get().runtimeDiagnostics_.Record(message);
		logger::info("[SourceDLSSG/SL {}] {}", static_cast<int>(a_type), message);
	}
	bool Backend::InitializeNvidia(const std::filesystem::path& directory)
	{
		if (nvidiaInitialized_) { return true; }
		if (!NvidiaAdapter()) { return Check(DXGI_ERROR_UNSUPPORTED, "NVIDIA provider requires NVIDIA hardware"); }
		MFGUnlock::StartupScope startupScope(mfgUnlock_);
		if (!Load(directory)) { return false; }
		void* upgraded = device12_.Get();
		mfgUnlock_.Prepare(device12_.Get(), directory_);
		device12_->AddRef();
		const auto upgradedResult = upgrade_(&upgraded);
		upgradedDevice12_.Attach(static_cast<ID3D12Device*>(upgraded));
		if (!Check(upgradedResult, "upgrade D3D12 device") || !upgradedDevice12_ ||
			!Check(setDevice_(device12_.Get()), "slSetD3DDevice")) { return false; }
		auto luid = device12_->GetAdapterLuid();
		sl::AdapterInfo info{};
		info.deviceLUID = reinterpret_cast<std::uint8_t*>(&luid);
		info.deviceLUIDSizeInBytes = sizeof(luid);
		for (const auto feature : { sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G }) {
			const auto operation = std::format("slIsFeatureSupported feature={}", feature);
			logger::info("[SourceDLSSG] startup stage={}", operation);
			if (!Check(supported_(feature, info), operation.c_str())) { return false; }
		}
		for (std::size_t index = 0; index < kStreamlineModules.size(); ++index) {
			const auto* name = kStreamlineModules[index];
			runtimeModules_[index] = TheosRenderPipeline::PluginPaths::RetainLoadedModule(directory_ / name);
			if (!runtimeModules_[index]) {
				const auto configured = directory_ / name;
				const auto attributes = GetFileAttributesW(configured.c_str());
				const bool exists = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
				const auto otherPath = PluginPaths::ModulePath(GetModuleHandleW(name));
				const bool overrideObserved = runtimeDiagnostics_.FrameGenerationOverrideObserved();
				logger::error("[SourceDLSSG] module retention failed configured={} filePresent={} sameNameLoadedPath={} fgOverrideObserved={}",
					configured.string(), exists, otherPath.empty() ? "none" : otherPath.string(), overrideObserved);
				Check(E_FAIL, RuntimeModuleFailureMessage(std::filesystem::path(name).string(), overrideObserved).c_str());
				return false;
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
			!resolve(sl::kFeatureDLSS_G, "slDLSSGSetOptions", api_.setOptions)) { return false; }
		mfgUnlock_.BindWrapper(reinterpret_cast<const void*>(api_.setOptions));
		nvidiaInitialized_ = true; return true;
	}
	bool Backend::UpgradeNvidiaFactory()
	{
		if (streamlineFactory_) { return true; }
		void* factory = nativeFactory_.Get();
		nativeFactory_.Get()->AddRef();
		const auto factoryResult = upgrade_(&factory);
		ComPtr<IDXGIFactory> upgradedFactory;
		upgradedFactory.Attach(static_cast<IDXGIFactory*>(factory));
		if (!Check(factoryResult, "upgrade factory") || !upgradedFactory) { return false; }
		streamlineFactory_ = upgradedFactory;
		return true;
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
		xefg_.SetLogger([](const char* text) { logger::info("[XeFG SDK] {}", text); });
		device11_->GetImmediateContext(&context11_);
		const auto diagnosticsPath = PluginPaths::Directory() / L"TheosRenderPipeline.Diagnostics.ini";
		deviceLoss_.Configure(DeviceLossDiagnostics::ReadDREDSetting(diagnosticsPath.c_str()));
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> adapter;
		if (!Check(device11_.As(&dxgiDevice), "D3D11 DXGI device") ||
			!Check(dxgiDevice->GetAdapter(&adapter), "D3D11 adapter") ||
			!Check(ReShadeIntegration::Get().CreateSourceDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, &device12_), "D3D12 device")) { return fault_; }
		DXGI_ADAPTER_DESC adapterDesc{};
		if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
			logger::info("[SourceDLSSG] rendering adapter={} vendor=0x{:04X} device=0x{:04X} LUID={:08X}:{:08X}",
				std::filesystem::path(adapterDesc.Description).string(), adapterDesc.VendorId, adapterDesc.DeviceId,
				static_cast<std::uint32_t>(adapterDesc.AdapterLuid.HighPart), adapterDesc.AdapterLuid.LowPart);
		}
		logger::info("[ReShade] {}", ReShadeIntegration::Get().Status());
		adapterVendor_ = adapterDesc.VendorId;
		nativeFactory_ = a_factory;
		const bool xeSSStartup = RenderPipeline::GetSingleton()->mUpscaleType == ::XeSS;
		const auto startup = SelectProviderStartup(xeSSStartup, NvidiaAdapter(), RequestedProvider());
		RequestProvider(startup.requested);
		provider_ = startup.presenter;
		const bool intelStartup = provider_ == FrameGenerationProvider::XeFG;
		// Keep the compatibility startup scope through swapchain creation and
		// the initial NVIDIA session, as on the accepted NVIDIA path.
		std::unique_ptr<MFGUnlock::StartupScope> nvidiaStartup;
		if (!intelStartup) { nvidiaStartup = std::make_unique<MFGUnlock::StartupScope>(mfgUnlock_); }
		if (intelStartup) {
			logger::info("[FrameGeneration] XeSS startup uses Intel XeFG/XeLL; NVIDIA runtimes are not initialized");
		} else if (!InitializeNvidia(a_directory)) { return fault_; }
		logger::info("[FrameGeneration] startup upscaler={} requested={} presenter={}",
			xeSSStartup ? "XeSS" : "DLSS/DLAA", ProviderName(startup.requested), ProviderName(startup.presenter));
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (!Check(device12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), "presenting queue") ||
			!Check(interop_.Initialize(device11_.Get(), device12_.Get(), queue_.Get()), "shared interop")) { return fault_; }
		if (FAILED(queue_->GetTimestampFrequency(&hdrTimestampFrequency_))) { hdrTimestampFrequency_ = 0; }
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (FAILED(queue_->GetTimestampFrequency(&neuralTimestampFrequency_)) || !neuralTimestampFrequency_) {
			neuralTimestampFrequency_ = 0;
			logger::warn("[DLSSNR Source] presenting queue does not expose GPU timestamp frequency; CPU boundary timing remains available");
		}
#endif
		if (!intelStartup && !UpgradeNvidiaFactory()) { return fault_; }
		auto desc = a_desc;
		window_ = a_desc.OutputWindow;
		{
			const auto hdr = HDROutputConfiguration();
			const char* reason = "not requested";
			if (hdr.enabled) {
				if (CommunityShaders::Active()) { reason = "Community Shaders owns HDR on this route"; }
				else if (!HDROutputEligibleFormat(a_desc.BufferDesc.Format)) { reason = "producer does not finish in 8-bit SDR"; }
				else { hdrNative_ = true; reason = "waiting for display state"; }
			}
			hdrRequested_ = hdr.enabled;
			std::scoped_lock lock(hdrMutex_);
			hdrState_.requested = hdr.enabled; hdrState_.native = hdrNative_; hdrState_.reason = reason;
			logger::info("[HDROutput] requested={} native={} gameFormat={} {}", hdr.enabled, hdrNative_,
				static_cast<int>(a_desc.BufferDesc.Format), reason);
		}
		desc.BufferDesc.Format = NativeFormat(a_desc.BufferDesc.Format);
		desc.BufferCount = 2;
		desc.Flags &= ~(0x2000u | 0x4000u);
		// The pinned DLSS-G runtime submits immediate Presents with ALLOW_TEARING,
		// including generation-off startup. DXGI requires this creation flag.
		desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		presenterDesc_ = desc;
		ComPtr<IDXGISwapChain> native;

		// Upscaling and presentation are independent. XeSS also needs its SR
		// context when the saved presenter is NVIDIA.
		if (xeSSStartup && !Check(xess_.Open(device12_.Get(),
			PluginPaths::Directory() / L"TheosRenderPipeline" / L"Intel"), "XeSS context")) { return fault_; }
		if (intelStartup) {
			const auto intelDirectory = PluginPaths::Directory() / L"TheosRenderPipeline" / L"Intel";
			if (!CheckXeFG(xefg_.Probe(device12_.Get(), intelDirectory)) ||
				!CheckXeFG(xefg_.Create(device12_.Get(), queue_.Get(), nativeFactory_.Get(), desc, true, &native, 0,
					XeFGConfiguration()))) { return fault_; }
			providerStatus_ = "Intel XeFG presenter; latency owner=XeLL";
		} else if (!Check(streamlineFactory_->CreateSwapChain(queue_.Get(), &desc, &native), "Streamline swapchain")) { return fault_; }
		retainedNative_ = native;
		PollDisplayHDR(true);
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
		session_.RequestUIRecomposition(UIRecompositionConfiguration());
		session_.RequestOutputFPSLimit(outputFPSLimit_.load(std::memory_order_relaxed));
		if (!intelStartup) {
			if (!CheckSession(session_.Start(api_, 1))) { return fault_; }
			nvidiaSessionStarted_ = true;
			mfgUnlock_.Tick();
			session_.SetMFGUnlockState(mfgUnlock_.Snapshot().UsesCompatibilityUnlock(), mfgUnlock_.Snapshot().Ready());
		}
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
		cameraAvailable_ = true;
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
		hdrFrameTagged_ = false;
		if (HDRSignalled()) {
			// DLSS-G needs HUD-less and UI in the backbuffer's HDR10 encoding.
			// BeforePresent writes these targets; without both layers, tag neither.
			const bool layers = a_ui && a_hudless && HDROutputEligibleFormat(hudless_.desc.Format) &&
				hudless_.desc.Width == a_width && hudless_.desc.Height == a_height &&
				ui_.desc.Width == a_width && ui_.desc.Height == a_height;
			guides.ui = {}; guides.hudless = {};
			if (layers) {
				if (!EnsureHDRTargets(a_width, a_height)) { return false; }
				auto tag12 = [](ID3D12Resource* resource) {
					const auto d = resource->GetDesc();
					TaggedTexture t;
					t.resource = sl::Resource(sl::ResourceType::eTex2d, resource, D3D12_RESOURCE_STATE_COMMON);
					t.resource.width = static_cast<UINT>(d.Width); t.resource.height = d.Height;
					t.extent = { 0, 0, static_cast<UINT>(d.Width), d.Height };
					return t;
				};
				guides.ui = tag12(hdrOutputPass_->UITarget());
				guides.hudless = tag12(hdrOutputPass_->HudlessTarget());
				hdrFrameTagged_ = true;
			}
		}
		guides.displayWidth = a_width; guides.displayHeight = a_height;
		if (nvidiaNeedsPresent_) { return true; } // Establish the new native presenter before Reflex markers.
		if (provider_ == FrameGenerationProvider::XeFG) {
			if (!CheckXeFG(xefg_.Prepare(frameConstants_, XeFGFrameTimeConfiguration()))) { return false; }
			recreateXeFG_ = xefg_.Snapshot().invertedDepth != (frameConstants_.depthInverted == sl::eTrue) ||
				xefg_.Snapshot().experimentalMFG != XeFGConfiguration().experimentalMFG;
			return true; // Intel copies the final HUD-less image after late NR, at Present.
		}
		if (!Check(interop_.SignalD3D11(Work::FrameGeneration), "D3D11 guides ready")) { return false; }
		ID3D12GraphicsCommandList* list = nullptr;
		if (!Check(interop_.Begin(Work::FrameGeneration, &list), "begin guide tags") ||
			!CheckSession(session_.Prepare(frameConstants_, guides, list)) ||
			!Check(interop_.Submit(Work::FrameGeneration), "submit guide tags")) { return false; }
		return true;
	}
	void Backend::RecordScreenshot(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_output, DXGI_COLOR_SPACE_TYPE a_colorSpace) noexcept
	{
		// Protect configuration/path allocations as well as GPU HRESULTs.
		ScreenshotBoundary([&] {
			if (screenshotCapture_.Busy() || screenshotWriter_.Busy()) { return S_OK; }
			ReShadeIntegration::ScreenshotRequest request;
			if (!ReShadeIntegration::Get().TakeScreenshotRequest(request)) { return S_OK; }
			if (!request.replaceAllowed) {
				logger::warn("[Screenshot] ReShade overlay open or UI frame unavailable; keeping ReShade's file {}. Close the ReShade overlay before taking a corrected screenshot.", request.path);
				return S_OK;
			}
			if (!FinalFrameCapture::Supports(a_output->GetDesc().Format, a_colorSpace)) {
				logger::warn("[Screenshot] Output format={} colour space={} is not converted; keeping ReShade's file {}",
					static_cast<unsigned>(a_output->GetDesc().Format), static_cast<unsigned>(a_colorSpace), request.path);
				return S_OK;
			}
			const auto result = screenshotCapture_.Record(device12_.Get(), a_list, a_output, a_colorSpace);
			if (FAILED(result)) {
				logger::warn("[Screenshot] final frame capture unavailable (0x{:08X}); keeping ReShade's file {}",
					static_cast<std::uint32_t>(result), request.path);
				return S_OK;
			}
			screenshotPath_ = std::move(request.path);
			screenshotQuality_ = request.jpegQuality;
			screenshotRecorded_ = true;
			return S_OK;
		});
	}
	void Backend::FinishScreenshot() noexcept
	{
		ScreenshotBoundary([&] {
			const auto written = screenshotWriter_.TakeResult();
			if (FAILED(written)) {
				logger::warn("[Screenshot] writer failed (0x{:08X}); rendering continues", static_cast<std::uint32_t>(written));
			} else if (written == S_OK) {
				logger::info("[Screenshot] final frame saved");
			}
			if (!screenshotCapture_.Busy()) { return S_OK; }
			FinalFrameCapture::Readback readback;
			const auto result = screenshotCapture_.Poll(readback);
			if (result == S_FALSE) { return S_OK; }
			if (FAILED(result)) {
				if (!screenshotFaultLogged_) {
					screenshotFaultLogged_ = true;
					logger::warn("[Screenshot] final frame readback failed (0x{:08X}); keeping ReShade's file {}",
						static_cast<std::uint32_t>(result), screenshotPath_);
				}
				return S_OK; // Retain the readback after a device fault.
			}
			screenshotWriter_.Start([readback = std::move(readback), path = std::move(screenshotPath_), quality = screenshotQuality_] {
				FinalFrameCapture::Image image;
				auto hr = FinalFrameCapture::Read(readback, image);
				if (SUCCEEDED(hr)) { hr = ScreenshotFile::Replace(std::filesystem::u8path(path), image.width, image.height, image.bgr, quality); }
				return hr;
			});
			screenshotPath_.clear();
			return S_OK;
		});
	}
	HRESULT Backend::BeforePresent(ID3D12Resource* a_source, ID3D12Resource* a_destination, DXGI_COLOR_SPACE_TYPE a_colorSpace)
	{
		if (!Ready()) { return FAILED(fault_) ? fault_ : E_UNEXPECTED; }
		FinishScreenshot();
		const auto oldReflexRequest = session_.Snapshot().reflexRequested;
		const auto oldReflexSubmitted = session_.Snapshot().reflexSubmitted;
		const auto oldFrameLimit = session_.Snapshot().frameLimitSubmittedUs;
		const auto oldGeneration = session_.Snapshot().generationRequested;
		const auto oldLimited = session_.Snapshot().generationLimited;
		const auto oldRecomposition = session_.Snapshot().options.enableUserInterfaceRecomposition;
		if (provider_ == FrameGenerationProvider::NVIDIA) { mfgUnlock_.Tick(); }
		session_.SetMFGUnlockState(mfgUnlock_.Snapshot().UsesCompatibilityUnlock(), mfgUnlock_.Snapshot().Ready());
		session_.RequestGeneration(GenerationConfiguration());
		session_.RequestReflexMode(ReflexConfiguration());
		session_.RequestUIRecomposition(UIRecompositionConfiguration());
		session_.RequestOutputFPSLimit(outputFPSLimit_.load(std::memory_order_relaxed));
		const bool prepared = provider_ == FrameGenerationProvider::XeFG ? xefg_.Snapshot().prepared :
			session_.Snapshot().stage == SessionStage::Rendering;
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
		DXGI_COLOR_SPACE_TYPE outputColorSpace = a_colorSpace;
		const bool rendererHDR = hdrNative_ && a_destination->GetDesc().Format == HDROutputPass::kOutputFormat &&
			HDROutputEligibleFormat(realSource->GetDesc().Format);
		const bool hdrEncoded = !rendererHDR && realSource->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
			a_destination->GetDesc().Format == DXGI_FORMAT_R10G10B10A2_UNORM;
		if (rendererHDR) {
			outputResult = RecordOutput(list, realSource, a_destination, prepared, outputColorSpace);
		} else if (hdrEncoded) {
			if (!hdrPass_) { hdrPass_ = std::make_unique<HDRPass>(); }
			outputResult = hdrPass_->Record(device12_.Get(), list, interop_.CurrentSlot(Work::SwapChain), realSource, a_destination);
			outputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		} else {
			outputResult = Interop::RecordCopy(list, realSource, a_destination);
		}
		const bool hdrLayersComposed = hdrFrameTagged_;
		hdrFrameTagged_ = false;
		if (SUCCEEDED(outputResult)) {
			// CS can deliver already-encoded PQ through an equal-format copy.
			// A conversion performed here is not the only source of HDR output.
			// Renderer-owned HDR keeps the finished SDR frame; save that instead of PQ.
			if (rendererHDR) { RecordScreenshot(list, realSource, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709); }
			else { RecordScreenshot(list, a_destination, outputColorSpace); }
		}
		const bool transitionBlocked = TransitionBlocked();
		const auto transitionWarmup = transitionWarmupPresents_.load(std::memory_order_acquire);
		const bool generationAllowed = enabled_ && !transitionBlocked && transitionWarmup == 0;
		if (provider_ == FrameGenerationProvider::XeFG) {
			const bool hdrLayers = hdrNative_ && hdrDisplay_ && hdrOutputPass_;
			auto* intelHudless = hdrLayers ? hdrOutputPass_->HudlessTarget() : hudless_.texture12.Get();
			// The same premultiplied UI layer DLSS-G recomposes. HDR output wrote
			// its HDR10 copy only when both layers were composed this frame.
			auto* intelUI = hdrLayers ? (hdrLayersComposed ? hdrOutputPass_->UITarget() : nullptr) :
				(uiSource_ ? ui_.texture12.Get() : nullptr);
			// No FP16/scRGB reinterpretation. XeFG copies matching native-format layers.
			const auto destination = a_destination->GetDesc();
			const bool compatible = intelHudless && intelHudless->GetDesc().Format == destination.Format;
			if (intelUI) {
				const auto ui = intelUI->GetDesc();
				if (ui.Format != destination.Format || ui.Width != destination.Width || ui.Height != destination.Height) { intelUI = nullptr; }
			}
			if (!CheckXeFG(xefg_.SetDebugView(XeFGOnlyGenerated(), XeFGTagGenerated()))) { return fault_; }
			if (!CheckXeFG(xefg_.BeforePresent(list, motion_.texture12.Get(), depth_.texture12.Get(), intelHudless, intelUI,
				generationAllowed && prepared && compatible, UIRecompositionConfiguration(),
				outputFPSLimit_.load(), XeFGConfiguration().generatedFrames))) { return fault_; }
		}
		if (!Check(outputResult, "record native output copy/conversion") ||
			!Check(interop_.Submit(Work::SwapChain), "submit native output copy")) { return fault_; }
		if (screenshotRecorded_) {
			screenshotRecorded_ = false;
			ScreenshotBoundary([&] {
				if (const auto result = screenshotCapture_.Submitted(queue_.Get()); FAILED(result)) {
					logger::warn("[Screenshot] final frame fence signal failed (0x{:08X}); keeping ReShade's file {}",
						static_cast<std::uint32_t>(result), screenshotPath_);
				}
				return S_OK;
			});
		}
		if (provider_ == FrameGenerationProvider::NVIDIA && !nvidiaNeedsPresent_ && ((prepared && !CheckSession(session_.CompleteInputWrites())) ||
			!CheckSession(session_.BeforePresent(generationAllowed)))) { return fault_; }
		if (provider_ == FrameGenerationProvider::XeFG && !CheckXeFG(xefg_.FinalizePresent())) { return fault_; }
		if (oldReflexRequest != session_.Snapshot().reflexRequested ||
			oldReflexSubmitted != session_.Snapshot().reflexSubmitted || oldFrameLimit != session_.Snapshot().frameLimitSubmittedUs) {
			logger::info("[SourceDLSSG] Reflex frame={} requested={} submitted={} generation={} outputLimitUs={}",
				FrameIndex(), ReflexModeName(session_.Snapshot().reflexRequested),
				ReflexModeName(session_.Snapshot().reflexSubmitted),
				GenerationEnabled(session_.Snapshot().options.mode), session_.Snapshot().frameLimitSubmittedUs);
		}
		if (oldGeneration != session_.Snapshot().generationRequested || oldLimited != session_.Snapshot().generationLimited) {
			const auto& s = session_.Snapshot();
			logger::info("[SourceDLSSG] MFG requestedGenerated={} requestedDynamic={} targetFPS={} submittedGenerated={} submittedMode={} maxGenerated={} limited={}",
				s.generationRequested.generatedFrames, s.generationRequested.dynamic, s.generationRequested.dynamicTargetFPS,
				s.options.numFramesToGenerate, static_cast<int>(s.options.mode), s.state.numFramesToGenerateMax, s.generationLimited);
		}
		if (oldRecomposition != session_.Snapshot().options.enableUserInterfaceRecomposition) {
			const auto& s = session_.Snapshot();
			logger::info("[SourceDLSSG] UI recomposition frame={} requested={} submitted={} mode={}",
				s.frameIndex, s.uiRecompositionRequested, s.options.enableUserInterfaceRecomposition == sl::eTrue,
				static_cast<int>(s.options.mode));
		}
#if !defined(TRP_NO_NEURAL_RENDERING)
		{
			std::scoped_lock lock(neuralMutex_);
			const bool changed = neuralSnapshot_.active != active;
			neuralSnapshot_.active = active;
			neuralSnapshot_.effectivePasses = options.EffectivePasses();
			neuralSnapshot_.passOverride = active ? options.passOverride : NeuralRendering::PassOverride::None;
			if (active) { ++neuralSnapshot_.evaluations; }
			if (reset) { ++neuralSnapshot_.resets; }
			if (active) { neuralSnapshot_.telemetry = neuralPass_->Telemetry(); }
			neuralSnapshot_.status = unavailable ? unavailable : active ? neuralPass_->Status() : options.enabled ?
				"NR waiting for world inputs and dedicated native UI; standard DLSS active" : "standard DLSS; source NR is off";
			if (changed || (active && (reset || neuralSnapshot_.evaluations % 600 == 0))) {
				logger::info("[SourceDLSSG NR] frame={} active={} evaluations={} reset={} style={} fgRequested={} {}",
					FrameIndex(), active, neuralSnapshot_.evaluations, reset,
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
		if (FAILED(a_result)) { return a_result; }
		providerSwitchPending_ = PreflightProviderSwitch();
		if (provider_ == FrameGenerationProvider::XeFG) {
			if (!CheckXeFG(xefg_.AfterPresent(a_result))) { return fault_; }
			// ONLY_NOW tags record Intel's input copies in the submitted SwapChain
			// list, whose completion also covers the late output/NR readers. The
			// Lab GPU wait queues that dependency before the next D3D11 frame writes
			// the singleton inputs; the default drains every host lane on the CPU.
			// Resize, recreation and teardown still drain.
			const bool gpuInputWait = XeFGGpuInputWait();
			if (gpuInputWait != xefgReuseWindowGpu_) {
				xefgReuseMs_.clear(); xefgSleepMs_.clear(); xefgReuseWindowGpu_ = gpuInputWait;
				logger::info("[XeFG] input reuse mode now {}", gpuInputWait ? "gpu-fence" : "cpu-drain");
			}
			const auto reuseStart = std::chrono::steady_clock::now();
			if (!Check(gpuInputWait ? interop_.WaitD3D11(Work::SwapChain) : interop_.Drain(),
				gpuInputWait ? "Intel ONLY_NOW input reuse GPU wait" : "retire Intel ONLY_NOW input copies")) { return fault_; }
			xefgReuseMs_.push_back(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - reuseStart).count());
			const auto& intel = xefg_.Snapshot();
			xefgSleepMs_.push_back(intel.sleepMs);
			if (xefgReuseMs_.size() >= 600) {
				auto percentiles = [](std::vector<float>& v) {
					std::sort(v.begin(), v.end());
					return std::array{ v[(v.size() - 1) / 2], v[(v.size() - 1) * 95 / 100], v.back() };
				};
				const auto reuse = percentiles(xefgReuseMs_), sleep = percentiles(xefgSleepMs_);
				logger::info("[XeFG] input reuse={} cpu ms p50={:.3f} p95={:.3f} max={:.3f}; XeLL sleep ms p50={:.3f} p95={:.3f} max={:.3f} (n={})",
					gpuInputWait ? "gpu-fence" : "cpu-drain", reuse[0], reuse[1], reuse[2], sleep[0], sleep[1], sleep[2], xefgReuseMs_.size());
				xefgReuseMs_.clear(); xefgSleepMs_.clear();
			}
			if (intel.presents <= 3 || intel.presents % 600 == 0) {
				logger::info("[XeFG] frame={} enabled={} presented={} totalOutputs={} generatedPresents={} interpolationResult={} warnings={} epoch={} uiTexture={} uiTexturePresents={} frameTimeMs={:.2f} onlyGenerated={} tagGenerated={} latency=XeLL",
					intel.frameId, intel.enabled, intel.framesPresented, intel.totalOutputs, intel.generatedPresents,
					intel.interpolationResult, intel.warnings, intel.epoch, intel.uiTexture, intel.uiTexturePresents, intel.frameTimeMs, intel.onlyGenerated, intel.tagGenerated);
			}
		} else if (nvidiaNeedsPresent_) {
			// The NVIDIA driver can retain the former Intel swapchain's latency
			// context until this new chain actually presents. Resuming PCL markers
			// immediately after CreateSwapChain reproducibly faults in NVAPI.
			// Use the next normal host output, never an extra/uninitialized Present.
			if (a_result != S_OK) { return a_result; }
			const auto resumeFrame = FrameIndex();
			if (!nvidiaSessionStarted_) {
				if (!CheckSession(session_.Start(api_, 1, resumeFrame))) { return fault_; }
				nvidiaSessionStarted_ = true;
				mfgUnlock_.Tick();
				session_.SetMFGUnlockState(mfgUnlock_.Snapshot().UsesCompatibilityUnlock(), mfgUnlock_.Snapshot().Ready());
			} else if (!CheckSession(session_.ResumeAfterResize(resumeFrame))) { return fault_; }
			nvidiaNeedsPresent_ = false;
			providerStatus_ = "NVIDIA presenter; latency owner=Reflex";
			logger::info("[FrameGeneration] NVIDIA replacement presented; Reflex resumed frame={}", FrameIndex());
		} else if (!CheckSession(session_.AfterPresent(true, false, providerSwitchPending_))) { return fault_; }
		// Between frames: the next Prepare and BeforePresent both see any new
		// display state, and the colour space changes before that frame's Present.
		if (SUCCEEDED(a_result)) { PollDisplayHDR(false); }
		if (SUCCEEDED(a_result) && !TransitionBlocked()) {
			auto remaining = transitionWarmupPresents_.load(std::memory_order_acquire);
			while (remaining && !transitionWarmupPresents_.compare_exchange_weak(
				remaining, remaining - 1, std::memory_order_acq_rel)) {}
			if (remaining == 1) {
				logger::info("[SourceDLSSG] transition warm-up complete; generation may resume on the next frame");
			}
		}
		const auto& state = session_.Snapshot();
		if (provider_ == FrameGenerationProvider::NVIDIA && (state.frameIndex <= 3 || state.frameIndex % 600 == 0)) {
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
		if (provider_ == FrameGenerationProvider::XeFG) {
			if (!CheckXeFG(xefg_.Disable())) { return false; }
		} else if (session_.Snapshot().stage != SessionStage::Stopped && !CheckSession(session_.Stop())) { return false; }
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
		hdrOutputPass_.reset();
		hdrFrameTagged_ = false;
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
	void Backend::PollDisplayHDR(bool a_force)
	{
		if (!hdrNative_) { return; }
		if (!a_force && hdrPollCountdown_ && --hdrPollCountdown_) { return; }
		hdrPollCountdown_ = 120;
		// Windows' SDR content brightness can change without a new DXGI factory.
		const bool refreshSDR = ++hdrSDRPolls_ % 5 == 0 && HDROutputConfiguration().matchWindowsSDR;
		const auto monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
		if (!a_force && hdrFactory_ && hdrFactory_->IsCurrent() && monitor == hdrMonitor_ && hdrColorSpaceApplied_) {
			if (refreshSDR) { RefreshSDRWhite(); }
			return;
		}
		// A fresh factory reports the current Windows HDR state of each output.
		const auto start = std::chrono::steady_clock::now();
		ComPtr<IDXGIFactory1> factory;
		const auto created = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		const auto display = SUCCEEDED(created) ? QueryDisplayHDR(factory.Get(), window_) : DisplayHDR{};
		const double queryUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		hdrFactory_ = factory;
		hdrMonitor_ = display.monitor ? display.monitor : monitor;
		hdrDeviceName_ = display.deviceName;
		const bool changed = display.active != hdrDisplay_ || !hdrColorSpaceApplied_;
		hdrDisplay_ = display.active;
		{
			std::scoped_lock lock(hdrMutex_);
			++hdrState_.displayQueries; hdrState_.displayQueryTotalUs += queryUs;
			hdrState_.displayQueryMaxUs = (std::max)(hdrState_.displayQueryMaxUs, queryUs);
			if (!a_force && (hdrState_.displayQueries <= 3 || hdrState_.displayQueries % 50 == 0)) {
				logger::info("[HDROutput] display re-query {} took {:.0f} us (factory was not current or the monitor changed)",
					hdrState_.displayQueries, queryUs);
			}
			hdrState_.display = hdrDisplay_; hdrState_.displayKnown = display.known;
			hdrState_.displayMaxNits = display.maxLuminance;
			hdrState_.reason = HDRDisplayReason(display.known);
		}
		if (HDROutputConfiguration().matchWindowsSDR) { RefreshSDRWhite(); }
		if (changed) {
			ApplyNativeColorSpace();
			logger::info("[HDROutput] display known={} hdr={} maxLuminance={:.0f} colourSpace={} factory=0x{:08X}",
				display.known, hdrDisplay_, display.maxLuminance, HDRSignalled() ? "PQ BT.2020" : "sRGB BT.709",
				static_cast<std::uint32_t>(created));
		}
	}
	const char* Backend::HDRDisplayReason(bool a_known) const
	{
		if (!hdrRequested_) { return "off; RGB10A2 swapchain kept until restart"; }
		return hdrDisplay_ ? "HDR10 output" : a_known ?
			"Windows HDR is off for this display; SDR output" : "display HDR state unavailable; SDR output";
	}
	bool Backend::UpdateHDRRequest(DXGI_FORMAT a_gameFormat)
	{
		const bool requested = HDROutputConfiguration().enabled;
		if (requested == hdrRequested_ || !Ready()) { return false; }
		const char* refused = !requested || hdrNative_ ? nullptr :
			CommunityShaders::Active() ? "Community Shaders owns HDR on this route" :
			!HDROutputEligibleFormat(a_gameFormat) ? "producer does not finish in 8-bit SDR" : nullptr;
		if (requested && !hdrNative_ && !refused) { return true; } // FinishHDRStart publishes the result.
		hdrRequested_ = requested;
		logger::info("[HDROutput] live requested={} native={} {}", requested, hdrNative_,
			refused ? refused : hdrNative_ ? "colour space follows the display" : "not requested");
		if (hdrNative_) {
			{ std::scoped_lock lock(hdrMutex_); hdrState_.requested = requested; }
			hdrColorSpaceApplied_ = false;
			PollDisplayHDR(true); // Signals the colour space for the next frame and publishes the reason.
			return false;
		}
		std::scoped_lock lock(hdrMutex_);
		hdrState_.requested = requested; hdrState_.reason = refused ? refused : "not requested";
		return false;
	}
	void Backend::FinishHDRStart(HRESULT a_reallocated, bool a_generation)
	{
		hdrRequested_ = true;
		if (FAILED(a_reallocated)) {
			logger::warn("[HDROutput] RGB10A2 swapchain reallocation failed (0x{:08X}); SDR output continues",
				static_cast<std::uint32_t>(a_reallocated));
			std::scoped_lock lock(hdrMutex_);
			hdrState_.requested = true; hdrState_.native = false;
			hdrState_.reason = "swapchain could not be reallocated for HDR; SDR output";
		} else {
			logger::info("[HDROutput] native swapchain reallocated as RGB10A2 without a restart");
			{ std::scoped_lock lock(hdrMutex_); hdrState_.requested = true; hdrState_.native = true; }
			hdrColorSpaceApplied_ = false;
			PollDisplayHDR(true);
		}
		// Quiesce stopped generation without the host knowing; resume it after
		// the same real-frame warm-up as a transition.
		enabled_ = a_generation;
		if (!TransitionBlocked()) { transitionWarmupPresents_.store(TransitionWarmupPresents, std::memory_order_release); }
	}
	void Backend::RefreshSDRWhite()
	{
		const auto start = std::chrono::steady_clock::now();
		const float queried = QuerySDRWhiteNits(hdrDeviceName_.data());
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		// A failed read during a display-config change keeps the last level for
		// the same display; a different display starts from its own reading.
		const bool sameDisplay = hdrSDRDevice_ == hdrDeviceName_;
		const float nits = queried > 0.0f || !sameDisplay ? queried : hdrSDRWhiteNits_;
		hdrSDRDevice_ = hdrDeviceName_;
		const bool changed = std::fabs(nits - hdrSDRWhiteNits_) >= 1.0f;
		hdrSDRWhiteNits_ = nits;
		std::scoped_lock lock(hdrMutex_);
		++hdrState_.sdrWhiteQueries;
		if (queried <= 0.0f) { ++hdrState_.sdrWhiteFailures; }
		hdrState_.sdrWhiteQueryMaxUs = (std::max)(hdrState_.sdrWhiteQueryMaxUs, us);
		hdrState_.windowsSDRWhiteNits = nits;
		if (changed) {
			logger::info("[HDROutput] Windows SDR content brightness={:.0f} nits display={} query={:.0f} us",
				nits, std::filesystem::path(hdrDeviceName_.data()).string(), us);
		}
	}
	void Backend::ApplyNativeColorSpace()
	{
		hdrColorSpaceApplied_ = false;
		ComPtr<IDXGISwapChain3> chain;
		if (!hdrNative_ || !retainedNative_ || FAILED(retainedNative_.As(&chain))) { return; }
		auto space = HDRSignalled() ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		UINT support{};
		HRESULT result = chain->CheckColorSpaceSupport(space, &support);
		if (SUCCEEDED(result) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) { result = chain->SetColorSpace1(space); }
		else if (SUCCEEDED(result)) { result = DXGI_ERROR_UNSUPPORTED; }
		if (FAILED(result) && HDRSignalled()) {
			logger::warn("[HDROutput] HDR10 colour space rejected (0x{:08X}); SDR output", static_cast<std::uint32_t>(result));
			hdrDisplay_ = false;
			space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
			result = chain->SetColorSpace1(space);
			std::scoped_lock lock(hdrMutex_);
			hdrState_.display = false; hdrState_.reason = "display rejected the HDR10 colour space; SDR output";
		}
		if (FAILED(result)) {
			logger::warn("[HDROutput] colour space {} not applied (0x{:08X})", static_cast<int>(space), static_cast<std::uint32_t>(result));
		}
		hdrColorSpaceApplied_ = SUCCEEDED(result);
	}
	bool Backend::EnsureHDRTargets(UINT a_width, UINT a_height)
	{
		if (!hdrOutputPass_) { hdrOutputPass_ = std::make_unique<HDROutputPass>(); }
		if (hdrOutputPass_->TargetsMatch(a_width, a_height)) { return true; }
		if (hdrOutputPass_->HudlessTarget() && (!Check(interop_.SignalD3D11(Work::FrameGeneration), "retire HDR output targets") ||
			!Check(interop_.Drain(), "drain HDR output targets"))) { return false; }
		return Check(hdrOutputPass_->CreateTargets(device12_.Get(), a_width, a_height), "HDR output target allocation");
	}
	HRESULT Backend::RecordOutput(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_source, ID3D12Resource* a_destination,
		bool a_prepared, DXGI_COLOR_SPACE_TYPE& a_colorSpace)
	{
		if (!hdrOutputPass_) { hdrOutputPass_ = std::make_unique<HDROutputPass>(); }
		if (hdrTimestampFrequency_) {
			if (const auto timing = hdrOutputPass_->EnableTiming(device12_.Get(), hdrTimestampFrequency_); FAILED(timing)) {
				logger::warn("[HDROutput] GPU timing unavailable (0x{:08X}); output continues", static_cast<std::uint32_t>(timing));
				hdrTimestampFrequency_ = 0;
			}
		}
		// Display state changes only between frames (AfterPresent), so tags,
		// encoding and colour space agree within a frame.
		const bool compose = a_prepared && hdrFrameTagged_;
		const bool hdr = compose || HDRSignalled();
		// A prepared world frame without both layers (for example native UI off)
		// still expands; menus and loading without generation stay at UI brightness.
		const bool expandWholeFrame = hdr && !compose && a_prepared;
		const auto constants = HDROutput::MakeShaderConstants(HDROutput::Effective(HDROutputConfiguration(), hdrSDRWhiteNits_),
			!hdr, expandWholeFrame);
		const auto slot = interop_.CurrentSlot(Work::SwapChain);
		const auto result = compose ?
			hdrOutputPass_->RecordCompose(device12_.Get(), a_list, slot, constants, a_source, ui_.texture12.Get(), hudless_.texture12.Get(), a_destination) :
			hdrOutputPass_->RecordEncode(device12_.Get(), a_list, slot, constants, a_source, a_destination);
		a_colorSpace = hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		if (SUCCEEDED(result)) {
			std::scoped_lock lock(hdrMutex_);
			++(compose ? hdrState_.composedFrames : hdrState_.encodedFrames);
			hdrState_.gpu.Add(hdrOutputPass_->TakeTiming());
			const auto total = hdrState_.composedFrames + hdrState_.encodedFrames;
			if (total <= 3 || total % 3600 == 0) {
				const auto& q = hdrState_;
				logger::info("[HDROutput] frames composed={} encoded={} hdr={} gpuSamples={} gpuAvgUs={:.1f} gpuMaxUs={:.1f} displayQueries={} displayQueryAvgUs={:.0f} displayQueryMaxUs={:.0f} sdrWhiteNits={:.0f} sdrWhiteQueries={} sdrWhiteQueryMaxUs={:.0f} {}",
					q.composedFrames, q.encodedFrames, hdr, q.gpu.samples, q.gpu.AverageUs(), q.gpu.maxUs, q.displayQueries,
					q.displayQueries ? q.displayQueryTotalUs / static_cast<double>(q.displayQueries) : 0.0, q.displayQueryMaxUs,
					q.windowsSDRWhiteNits, q.sdrWhiteQueries, q.sdrWhiteQueryMaxUs, q.reason);
			}
		}
		return result;
	}
	bool Backend::ResumeAfterResize()
	{
		if (nvidiaNeedsPresent_) { return Ready(); }
		if (provider_ == FrameGenerationProvider::XeFG) { xefg_.ResetHistory(); return Ready(); }
		return Ready() && CheckSession(session_.ResumeAfterResize());
	}

	bool Backend::PreflightProviderSwitch()
	{
		if (nvidiaNeedsPresent_) { return false; }
		const auto request = RequestedProvider();
		if ((request == provider_ && !recreateXeFG_) || !cameraAvailable_ || TransitionBlocked()) { return false; }
		if (request == FrameGenerationProvider::NVIDIA && !NvidiaAdapter()) {
			providerStatus_ = "NVIDIA frame generation requires NVIDIA hardware; Intel presenter retained";
			RequestProvider(provider_); return false;
		}
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (request == FrameGenerationProvider::NVIDIA &&
			NvidiaSwitchNeedsRestart(nvidiaInitialized_, NeuralRendering::PublicNGXInitializedInProcess())) {
			providerStatus_ = "NVIDIA frame generation needs a restart: NR started NVIDIA NGX before Streamline in this "
				"XeSS session. Intel presenter retained; save NVIDIA and restart, or switch before enabling NR.";
			logger::warn("[FrameGeneration] {}", providerStatus_);
			RequestProvider(provider_); return false;
		}
#endif
		if (request == FrameGenerationProvider::XeFG) {
			const bool compatible = XeFGPresenter::SupportsFormat(presenterDesc_.BufferDesc.Format) &&
				hudless_.texture12 && XeFGPresenter::SupportsFormat(hudless_.desc.Format);
			const auto result = compatible ? xefg_.Probe(device12_.Get(),
				PluginPaths::Directory() / L"TheosRenderPipeline" / L"Intel") : DXGI_ERROR_UNSUPPORTED;
			if (FAILED(result)) {
				providerStatus_ = std::format("XeFG unavailable: {} (0x{:08X}); current presenter retained",
					compatible ? xefg_.Status() : "requires compatible SDR/HDR10 inputs", static_cast<unsigned>(result));
				logger::warn("[FrameGeneration] {}", providerStatus_);
				RequestProvider(provider_); recreateXeFG_ = false;
				return false;
			}
		}
		return true;
	}
	HRESULT Backend::CreatePresenter(FrameGenerationProvider provider, IDXGISwapChain** result)
	{
		std::unique_ptr<MFGUnlock::StartupScope> nvidiaStartup;
		if (provider == FrameGenerationProvider::NVIDIA) {
			nvidiaStartup = std::make_unique<MFGUnlock::StartupScope>(mfgUnlock_);
			if (!InitializeNvidia(SourceFrameGeneration::GetSingleton()->settings.sourceDLSSGStreamlineDirectory) ||
				!UpgradeNvidiaFactory()) { return fault_; }
		}
		return provider == FrameGenerationProvider::XeFG ?
			xefg_.Create(device12_.Get(), queue_.Get(), nativeFactory_.Get(), presenterDesc_,
				frameConstants_.depthInverted == sl::eTrue, result, session_.Snapshot().frameIndex, XeFGConfiguration()) :
			streamlineFactory_->CreateSwapChain(queue_.Get(), &presenterDesc_, result);
	}
	bool Backend::ApplyProviderSwitch(SwapChain& wrapper)
	{
		if (!Ready()) { return false; }
		if (!providerSwitchPending_) {
			return provider_ != FrameGenerationProvider::XeFG || CheckXeFG(xefg_.BeginFrame());
		}
		const auto next = RequestedProvider();
		if (provider_ == FrameGenerationProvider::XeFG && !CheckXeFG(xefg_.Disable())) { return false; }
		if (!Check(interop_.SignalD3D11(Work::SwapChain), "last producer work before provider change") ||
			!Check(interop_.WaitForInputReaders(nullptr, 0), "provider queue retirement bridge") ||
			!Check(interop_.Drain(), "retire shared work before provider change")) { return false; }
		// Keep D3D11 facade/shared buffers, DLSS and NR alive. Only native buffers
		// and the provider-specific inner swapchain retire here. After this point
		// a failure is terminal; do not claim the discarded presenter is restored.
		wrapper.ReleaseInnerAfterRetirement();
		retainedNative_.Reset();
		if (provider_ == FrameGenerationProvider::XeFG && !CheckXeFG(xefg_.Destroy())) { return false; }
		ComPtr<IDXGISwapChain> replacement;
		if (!Check(CreatePresenter(next, &replacement), "create replacement presenter")) { return false; }
		retainedNative_ = replacement;
		if (!Check(wrapper.AttachInner(replacement.Get()), "attach replacement presenter")) { return false; }
		provider_ = next; recreateXeFG_ = providerSwitchPending_ = false;
		ResetPresentationFeedback();
		transitionWarmupPresents_.store(TransitionWarmupPresents);
		neuralHistory_.Invalidate();
		hdrColorSpaceApplied_ = false; ApplyNativeColorSpace();
		nvidiaNeedsPresent_ = next == FrameGenerationProvider::NVIDIA;
		providerStatus_ = std::format("{} presenter; latency owner={}", ProviderName(next), next == FrameGenerationProvider::XeFG ? "XeLL" : "Reflex");
		if (nvidiaNeedsPresent_) { providerStatus_ = "NVIDIA replacement waiting for first Present; Reflex suspended"; }
		logger::info("[FrameGeneration] provider change complete {}; shared DLSS/NR resources retained", providerStatus_);
		return true;
	}
}
