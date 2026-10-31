#include "ReShadeIntegration.h"
#include "../EngineScreenshot.h"
#include "SourceDLSSGBackend.h"
#include "XeFGLayerSelection.h"
#include <PCH.h>
#include "NeuralFrameEligibility.h"
#include "CommunityShaderIntegration.h"
#include "SourceDLSSGSwapChain.h"
#include "PresenterReleaseProbe.h"
#include "SourceRuntimeModuleDiagnostic.h"
#include "DlssgOverride.h"
#include "DlssgLiteBlending.h"
#include "StreamlineRuntimeFolder.h"
#include "../PluginPaths.h"
#include "../NeuralObjectHooks.h"
#include "../ScreenshotFile.h"
#include "../RenderPipeline.h"
#include "../PerformanceTuning.h"
#include "SourceFrameGeneration.h"
#include "StartupFallback.h"
#include <d3dcompiler.h>
#include <chrono>
#include <thread>
#if !defined(TRP_NO_NEURAL_RENDERING)
#include "NeuralRenderingFeatureSession.h"
#include "NeuralResearch.h"
#include "NeuralAdmission.h"
#include "NeuralRuntimeCompatibility.h"
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
	void Backend::ConfigurePreset(DlssgPreset a_preset)
	{
		const auto old = preset_.exchange(a_preset, std::memory_order_relaxed);
		LiteBlending::Configure(UsesLiteBlending(a_preset));
		if (old != a_preset && UsesUIRecomposition(old) == UsesUIRecomposition(a_preset)) {
			releaseForPreset_.store(true, std::memory_order_relaxed);
		}
	}
	Backend& Backend::Get()
	{
		// Streamline owns background work and DXGI proxies. Keep its module and
		// devices alive until process exit; no uncertain DLL-unload ordering.
		static auto* backend = new Backend;
		return *backend;
	}
	bool Backend::Check(HRESULT a_result, const char* a_operation, const char* a_source)
	{
		// Interop Begin/Drain results are checked immediately, so a pending
		// extended wait belongs to this operation. A crawling fence is a slow
		// GPU; a frozen one that times out suggests a stalled dependency.
		const auto& failure = interop_.LastFailure();
		RetirementWaitDiagnostics wait;
		const bool extendedWait = interop_.TakeExtendedWait(wait);
		const auto routing = RouteCheck(a_result, fault_, failure, extendedWait ? &wait : nullptr);
		if (routing.logExtendedWait) {
			logger::warn("[{}] {} waited {} ms ({} slices) for {} retirement target={} completed={}->{} result=0x{:08X}",
				a_source, a_operation, wait.elapsedMs, wait.slices, WorkName(wait.work),
				wait.target, wait.completedAtStart, wait.completedAtEnd, static_cast<std::uint32_t>(wait.result));
		}
		if (SUCCEEDED(a_result)) { return true; }
		if (routing.latch) {
			fault_ = a_result;
			status_ = std::format("{} failed HRESULT=0x{:08X}", a_operation, static_cast<std::uint32_t>(a_result));
			logger::error("[{}] {}", a_source, status_);
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
		return Check(result, xefg_.Status().c_str(), "XeFG");
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
			// NVIDIA's hint for whether to offer VSync with DLSS-G; logged for diagnosis only.
			if (s.stateQueries && int(s.state.bIsVsyncSupportAvailable) != reportedVSyncHint_) {
				reportedVSyncHint_ = int(s.state.bIsVsyncSupportAvailable);
				logger::info("[SourceDLSSG] DLSS-G reports VSync support available={} frame={}", reportedVSyncHint_ != 0, s.frameIndex);
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
		mfgUnlock_.SelectAdapter(device12_.Get());
		// RTX 20 needs the 310.9.1 set; RTX 30 also runs the newer DLSS-G unless kept on it.
		if (UsesBackportStreamline(mfgUnlock_.Snapshot().UsesTuringUnlock(), mfgUnlock_.Snapshot().UsesAmpereUnlock(), rtx30LegacyDLSSG_)) {
			const auto backport = BackportStreamlineDirectory(directory_);
			std::error_code error;
			if (std::filesystem::is_regular_file(backport / L"nvngx_dlssg.dll", error) &&
				std::filesystem::is_regular_file(backport / L"sl.interposer.dll", error)) {
				logger::info("[SourceDLSSG] {} uses the DLSS-G 310.9.1 set: {}",
					mfgUnlock_.Snapshot().UsesTuringUnlock() ? "RTX 20" : "RTX 30 (RTX30LegacyDLSSG)", backport.string());
				directory_ = backport;
			} else {
				logger::warn("[SourceDLSSG] DLSS-G 310.9.1 folder {} is missing; using {}", backport.string(), directory_.string());
			}
		}
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
		requirements_ = reinterpret_cast<PFun_slGetFeatureRequirements*>(::GetProcAddress(interposer_, "slGetFeatureRequirements"));
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
		if (!NvidiaFeaturesAvailable()) { return Check(DXGI_ERROR_UNSUPPORTED, "NVIDIA provider requires RTX hardware"); }
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
		ReadDLSSGRequirements();
		for (std::size_t index = 0; index < kStreamlineModules.size(); ++index) {
			const auto* name = kStreamlineModules[index];
			runtimeModules_[index] = TheosRenderPipeline::PluginPaths::RetainLoadedModule(directory_ / name);
			if (!runtimeModules_[index] && std::wstring_view(name) == L"nvngx_dlssg.dll" &&
				(runtimeModules_[index] = RetainNvidiaOverrideDlssg())) {
				// NVIDIA app's Frame Generation override supplied its own DLSS-G build.
				logger::warn("[SourceDLSSG] NVIDIA app frame generation override: DLSS-G loaded from {} instead of {}",
					PluginPaths::ModulePath(runtimeModules_[index]).string(), (directory_ / name).string());
				continue;
			}
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
		// Before slDLSSGSetOptions creates the feature; harmless on builds without Lite blending.
		LiteBlending::Install(runtimeModules_[0]);
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
	void Backend::ReadDLSSGRequirements()
	{
		sl::FeatureRequirements requirements{};
		const auto result = requirements_ ? requirements_(sl::kFeatureDLSS_G, requirements) : sl::Result::eErrorMissingOrInvalidAPI;
		if (result != sl::Result::eOk) {
			logger::info("[SourceDLSSG] DLSS-G requirements unavailable result={}; VSync Auto follows the game", static_cast<int>(result));
			return;
		}
		using Flags = sl::FeatureRequirementFlags;
		const auto has = [&](Flags flag) { return requirements.flags & flag; };
		dlssgVSyncOffRequired_.store(has(Flags::eVSyncOffRequired), std::memory_order_relaxed);
		logger::info("[SourceDLSSG] DLSS-G requirements flags=0x{:X} vsyncOffRequired={} hardwareSchedulingRequired={} "
			"driver={} (requires {}) OS={} (requires {}); VSync Auto {}",
			static_cast<std::uint32_t>(requirements.flags), has(Flags::eVSyncOffRequired), has(Flags::eHardwareSchedulingRequired),
			requirements.driverVersionDetected.toStr(), requirements.driverVersionRequired.toStr(),
			requirements.osVersionDetected.toStr(), requirements.osVersionRequired.toStr(),
			has(Flags::eVSyncOffRequired) ? "turns VSync off while DLSS-G generates" : "follows the game");
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
	bool Backend::EnterStartupTransport()
	{
		if (!device12_ || !CanUseStartupTransport(fault_, device12_->GetDeviceRemovedReason(), presenterAttempted_)) { return false; }
		startupFailure_ = status_;
		startupTransport_ = true;
		// This is a new, explicit transport state, not a repaired Streamline
		// session. Retain all partial SDK state; never call slInit again here.
		fault_ = S_OK;
		providerStatus_ = std::format("Normal presentation; frame generation, Reflex and NR unavailable: {}. Restart to retry.", startupFailure_);
		logger::warn("[FrameGeneration] {}", providerStatus_);
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
		if (ReShadeIntegration::Get().Blocked()) { logger::warn("[ReShade] {}", ReShadeIntegration::Get().Status()); }
		else { logger::info("[ReShade] {}", ReShadeIntegration::Get().Status()); }
		adapterVendor_ = adapterDesc.VendorId;
#if !defined(TRP_NO_NEURAL_RENDERING)
        neuralCapability_ = NeuralRendering::QueryNeuralHardware(adapterDesc.AdapterLuid, NeuralRendering::RuntimeKernelsSupported());
        neuralComputeCapability_ = neuralCapability_.architecture;
        pascalResearch_ = neuralCapability_.Pascal();
        if (pascalResearch_) { logger::info("[NR Pascal] native presentation via Intel host; DLSS-G unavailable; research pack={}", NeuralRendering::PascalConfiguration().pack.string()); }
#endif
		adapterDevice_ = adapterDesc.DeviceId;
		providerCapabilitiesKnown_ = true;
		if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &adapterDriver_))) { adapterDriver_ = {}; }
		nativeFactory_ = a_factory;
		// With Community Shaders, CS upscales; UpscaleType only selects TRP's own upscaler.
		const bool communityShaders = CommunityShaders::Active();
		// The host has already replaced DLSS/DLAA with XeSS on other GPUs.
		const bool xeSSStartup = !communityShaders && RenderPipeline::GetSingleton()->mUpscaleType == ::XeSS;
		const auto startup = SelectProviderStartup(xeSSStartup, NvidiaFrameGenerationAvailable(), RequestedProvider());
		RequestProvider(startup.requested);
		provider_ = startup.presenter;
		const bool intelStartup = provider_ == FrameGenerationProvider::XeFG;
		// Keep the compatibility startup scope through swapchain creation and
		// the initial NVIDIA session, as on the accepted NVIDIA path.
		std::unique_ptr<MFGUnlock::StartupScope> nvidiaStartup;
		if (!intelStartup) { nvidiaStartup = std::make_unique<MFGUnlock::StartupScope>(mfgUnlock_); }
		if (intelStartup) {
			logger::info("[FrameGeneration] Intel startup uses XeFG/XeLL; NVIDIA runtimes are not initialized");
		} else if (!InitializeNvidia(a_directory) && !EnterStartupTransport()) { return fault_; }
		logger::info("[FrameGeneration] startup upscaler={} requested={} presenter={}",
			communityShaders ? "Community Shaders" : xeSSStartup ? "XeSS" : "DLSS/DLAA",
			ProviderName(startup.requested), ProviderName(startup.presenter));
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (!Check(device12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), "presenting queue") ||
			!Check(interop_.Initialize(device11_.Get(), device12_.Get(), queue_.Get()), "shared interop")) { return fault_; }
		interop_.Timer().SetSink([](void*, const D3D12StageSample& sample) {
			PerformanceTuning::GetSingleton()->RecordD3D12Stage(sample.stage, sample.ms, sample.frame, sample.bounded);
		}, nullptr);
#if !defined(TRP_NO_NEURAL_RENDERING)
		{
			// Automatic chose 310.8 before the GPU was known. No NR feature exists yet.
			std::scoped_lock lock(neuralMutex_);
			const auto before = neuralOptions_.runtimePath;
			if (const auto* note = SelectNeuralRuntime(neuralOptions_)) { neuralSnapshot_.fallback = *note; }
			if (neuralOptions_.runtimePath != before) { neuralAvailability_.Reset(); }
		}
#endif
		if (FAILED(queue_->GetTimestampFrequency(&hdrTimestampFrequency_))) { hdrTimestampFrequency_ = 0; }
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (FAILED(queue_->GetTimestampFrequency(&neuralTimestampFrequency_)) || !neuralTimestampFrequency_) {
			neuralTimestampFrequency_ = 0;
			logger::warn("[DLSSNR Source] presenting queue does not expose GPU timestamp frequency; CPU boundary timing remains available");
		}
#endif
		if (!intelStartup && !startupTransport_ && !UpgradeNvidiaFactory() && !EnterStartupTransport()) { return fault_; }
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
		if (xeSSStartup &&
			!Check(xess_.Open(device12_.Get(), PluginPaths::IntelDirectory()), "XeSS context", "XeSS")) { return fault_; }
		presenterAttempted_ = true; // Never fall back after any presenter factory call, even if it returns failure.
		if (startupTransport_) {
			if (!Check(nativeFactory_->CreateSwapChain(queue_.Get(), &desc, &native), "normal startup transport")) { return fault_; }
		} else if (intelStartup) {
			if (!CheckXeFG(xefg_.Probe(device12_.Get(), PluginPaths::IntelDirectory(), xefgTensorRequested_)) ||
				!CheckXeFG(xefg_.Create(device12_.Get(), queue_.Get(), nativeFactory_.Get(), desc, XeFGStartupInvertedDepth(communityShaders), &native, 0,
					XeFGConfiguration(), IntelPipelines(XeFGPipelineCache(), xefg_.PipelineCacheName(), xefg_.RuntimeModule())))) { return fault_; }
			providerStatus_ = "Intel XeFG presenter; latency owner=XeLL";
		} else if (!Check(streamlineFactory_->CreateSwapChain(queue_.Get(), &desc, &native), "Streamline swapchain")) { return fault_; }
		retainedNative_ = native;
		PollDisplayHDR(true);
		api_.context = this;
		api_.waitForInputReaders = [](void* context, void* fence, std::uint64_t value) {
			auto& owner = *static_cast<Backend*>(context);
			// Called after Present: the snapshot still holds that frame's mode.
			const bool overlap = owner.NvidiaOverlapNextFrame();
			if (overlap != owner.nvidiaOverlapLogged_) {
				owner.nvidiaOverlapLogged_ = overlap;
				logger::info("[SourceDLSSG] next-frame D3D11 wait for frame-generation inputs now {}",
					overlap ? "deferred to the first shared write (Lab)" : "at the start of the frame");
			}
			const auto result = owner.interop_.WaitForInputReaders(static_cast<ID3D12Fence*>(fence), value,
				owner.session_.Snapshot().GenerationActive(), overlap);
			if (FAILED(result)) {
				const auto& detail = owner.interop_.LastInputWait();
				logger::error("[SourceDLSSG] input reuse fence stage={} fence={} value={} hostIdentity={} referenceFenceOwner={} inputFenceOwner={}",
					detail.stage, fence, value, detail.hostIdentity, detail.referenceFenceOwner, detail.inputFenceOwner);
			}
			return owner.Check(result, "input reuse fence");
		};
		session_.RequestReflexMode(ReflexConfiguration());
		session_.RequestUIRecomposition(UIRecompositionConfiguration());
		session_.RequestOnlyGenerated(XeFGOnlyGenerated());
		// Steady measures first; only a Fixed cap applies from the first frame.
		session_.RequestOutputFPSLimit(static_cast<FrameLimitMode>(frameLimit_.load(std::memory_order_relaxed)) == FrameLimitMode::Fixed ?
			outputFPSLimit_.load(std::memory_order_relaxed) : 0);
		if (!intelStartup && !startupTransport_) {
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
		status_ = startupTransport_ ? providerStatus_ : "source Streamline swapchain ready; generation off until valid inputs";
		logger::info("[SourceDLSSG] {} {}x{} format={} buffers=2", status_, desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format));
		*a_result = wrapper;
		return S_OK;
	}
	void Backend::ReleaseGuides()
	{
		motion_ = {}; depth_ = {}; ui_ = {}; hudless_ = {}; earlyNeuralColor_ = {}; objectMask_ = {}; objectMaskCurrent_ = false;
		uiSource_.Reset(); depthCopy_.ResetViews();
	}
    bool Backend::CopyObjectMask(FrameExtent extent)
    {
        objectMaskCurrent_ = false;
#if !defined(TRP_NO_NEURAL_RENDERING)
        if (!frameNeuralOptions_.enabled ||
            !NeuralRendering::ObjectsInUse(frameNeuralOptions_.objectMasks, frameNeuralOptions_.EffectivePasses())) { return true; }
        auto* source = NeuralObjects::Current();
        if (!source || objectMaskRefused_) { return true; }
        // Masks are experimental: a mask texture that cannot be shared turns them
        // off for the session and the frame continues with normal NR.
        if (!EnsureGuide(source,objectMask_,DXGI_FORMAT_R32_FLOAT,extent,true)) {
            if (FAILED(fault_)) { return false; }
            objectMaskRefused_ = true;
            logger::warn("[NR Objects] shared classification texture unavailable; object masks off for this session, normal NR continues");
            return true;
        }
        if (!Check(interop_.CopyInputRegion(source,objectMask_,extent),"NR object-classification copy")) { return false; }
        objectMaskCurrent_ = true;
#endif
        return true;
    }
	bool Backend::EnsureGuide(ID3D11Texture2D* a_source, SharedTexture& a_pair, DXGI_FORMAT a_format, FrameExtent a_extent, bool a_optional)
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
		const auto allocation = interop_.CreateSharedTexture(desc, a_pair, a_optional);
		if (FAILED(allocation)) {
			logger::error("[SourceDLSSG] shared guide format={} size={}x{} bind=0x{:X} mip={} array={} result=0x{:08X}",
				static_cast<int>(desc.Format), desc.Width, desc.Height, desc.BindFlags, desc.MipLevels, desc.ArraySize,
				static_cast<std::uint32_t>(allocation));
			if (a_optional) { a_pair = {}; return false; }
		}
		return Check(allocation, "shared guide allocation");
	}
	bool Backend::CopyDepth(ID3D11Texture2D* a_depth)
	{
		return Check(interop_.BeforeSharedWrites(), "deferred input wait before depth copy") &&
			Check(depthCopy_.Copy(context11_.Get(), a_depth, depth_.texture11.Get(),
			{depth_.desc.Width, depth_.desc.Height}), "depth copy");
	}
	bool Backend::Prepare(const sl::Constants& a_constants, ID3D11Texture2D* a_motion, ID3D11Texture2D* a_depth,
		ID3D11Texture2D* a_ui, ID3D11Texture2D* a_hudless, FrameExtent a_renderExtent,
		UINT a_width, UINT a_height, bool a_neuralEligible)
	{
		if (startupTransport_) { return Ready(); } // Plain output only; no vendor frame inputs are consumed.
		if (!Ready() || presenterReplacement_.Pending() || !Session::ValidConstants(a_constants) || !a_motion || !a_depth ||
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
		// NR before upscaling consumes only the world frame. Its early evaluation
		// may run where the interface is hidden (tm/console) or where this frame
		// falls back to Present, so neither DLSS-G guide is required for it.
		const bool worldOnly = frameNeuralOptions_.WorldOnly();
		const bool eligible = a_neuralEligible && !TransitionBlocked() &&
			NeuralPreparedEligible(worldOnly, neuralEvaluatedEarly_, a_hudless != nullptr, a_ui != nullptr);
		// The game host freezes settings before DLSS. Standalone callers that
		// only Prepare retain the late-stage contract and never run early NR here.
		if (!neuralFrameBegun_) {
			frameNeuralOptions_ = NeuralConfiguration();
#if !defined(TRP_NO_NEURAL_RENDERING)
			frameNeuralModelChanged_ = neuralPass_ && neuralPass_->ModelChangePending(frameNeuralOptions_);
#endif
			neuralEligible_ = eligible && !frameNeuralOptions_.WorldOnly();
			frameNeuralReset_ = neuralHistory_.ResetFor(frameNeuralOptions_, neuralEligible_, a_constants.reset == sl::eTrue);
		} else {
			neuralEligible_ = neuralEligible_ && eligible;
			frameNeuralReset_ |= a_constants.reset == sl::eTrue;
			if (!neuralEligible_) { neuralHistory_.Invalidate(); }
		}
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (frameNeuralOptions_.enabled && neuralEligible_ && neuralPass_ &&
			neuralPass_->NeedsResourceRecreation(frameNeuralOptions_, motion_.desc.Width, motion_.desc.Height)) { frameNeuralReset_ = true; }
#endif
		if (frameNeuralReset_ || frameNeuralModelChanged_) { frameConstants_.reset = sl::eTrue; }
        if (!neuralEvaluatedEarly_ && !CopyObjectMask(a_renderExtent)) { return false; }
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
		if (hdrNative_) {
			// FG needs layers in the backbuffer format even when this retained
			// RGB10A2 swapchain carries SDR (Windows HDR off or disabled live).
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
		if (nativePresenterFallback_) { fallbackPrepared_ = true; return true; }
		if (nvidiaNeedsPresent_) { return true; } // Establish the new native presenter before Reflex markers.
		if (provider_ == FrameGenerationProvider::XeFG) {
			if (!CheckXeFG(xefg_.Prepare(frameConstants_, XeFGFrameTimeConfiguration()))) { return false; }
			recreateXeFG_ = xefg_.Snapshot().invertedDepth != (frameConstants_.depthInverted == sl::eTrue) ||
				XeFGNeedsRecreation(xefg_.Snapshot().experimentalMFG, XeFGConfiguration()) ||
				xefg_.ProbedTensorRequest() != xefgTensorRequested_.load();
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
			const char* owner = "ReShade's";
			if (!ReShadeIntegration::Get().TakeScreenshotRequest(request)) {
				// Skyrim's own screenshot (PrintScreen, Photo Mode), saved at the render
				// size during this frame: replace it with this Present's finished frame.
				EngineScreenshot::Request engine;
				if (!EngineScreenshot::Take(engine)) { return S_OK; }
				// No path: Skyrim did not save it (Photo Mode); the writer finds the photo.
				owner = engine.path.empty() ? "the photo's" : "Skyrim's";
				request.path = engine.path;
				request.jpegQuality = engine.jpegQuality;
				request.replaceAllowed = true;
				screenshotPhoto_ = std::move(engine);
			}
			if (!request.replaceAllowed) {
				logger::warn("[Screenshot] ReShade overlay open or UI frame unavailable; keeping ReShade's file {}. Close the ReShade overlay before taking a corrected screenshot.", request.path);
				return S_OK;
			}
			if (!FinalFrameCapture::Supports(a_output->GetDesc().Format, a_colorSpace)) {
				logger::warn("[Screenshot] Output format={} colour space={} is not converted; keeping {} file {}",
					static_cast<unsigned>(a_output->GetDesc().Format), static_cast<unsigned>(a_colorSpace), owner, request.path);
				return S_OK;
			}
			const auto result = screenshotCapture_.Record(device12_.Get(), a_list, a_output, a_colorSpace);
			if (FAILED(result)) {
				logger::warn("[Screenshot] final frame capture unavailable (0x{:08X}); keeping {} file {}",
					static_cast<std::uint32_t>(result), owner, request.path);
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
					logger::warn("[Screenshot] final frame readback failed (0x{:08X}); keeping the original file {}",
						static_cast<std::uint32_t>(result), screenshotPath_);
				}
				return S_OK; // Retain the readback after a device fault.
			}
			screenshotWriter_.Start([readback = std::move(readback), path = std::move(screenshotPath_), quality = screenshotQuality_,
				photo = std::move(screenshotPhoto_)] {
				FinalFrameCapture::Image image;
				auto hr = FinalFrameCapture::Read(readback, image);
				if (FAILED(hr)) { return hr; }
				auto file = std::filesystem::u8path(path);
				PhotoFiles::File matched;
				if (path.empty()) {
					// Photo Mode writes its photo itself, about when the flag clears.
					matched = EngineScreenshot::FindPhoto(photo, std::chrono::seconds(5));
					if (!matched) {
						logger::info("[Screenshot] no unique completed {}x{} photo matched this request; original files retained",
							photo.width, photo.height);
						return S_FALSE;
					}
					file = matched.Path();
					logger::info("[Screenshot] replacing {} with the finished frame", file.filename().string());
				}
				if (!path.empty()) {
					// Exact-path Skyrim/ReShade screenshots retain the ordinary writer.
					return ScreenshotFile::Replace(file, image.width, image.height, image.bgr, quality);
				}
				return ScreenshotFile::Replace(file, image.width, image.height, image.bgr, quality,
					[&] { return matched.StillMatches(); });
			});
			screenshotPath_.clear();
			return S_OK;
		});
	}
	VSyncConfiguration Backend::PresentationConfiguration() const
	{
		bool meter = false;
		if (provider_ == FrameGenerationProvider::XeFG && XeFGFlipMetering()) {
			const auto flags = xefg_.PacingStats().flags;
			meter = XeFGPacing::CanMeter(flags);
		}
		const bool required = provider_ == FrameGenerationProvider::NVIDIA && DLSSGVSyncOffRequired() &&
			!startupTransport_ && !nativePresenterFallback_ && session_.Snapshot().GenerationActive();
		return {VSyncConfigurationMode(), meter, (presenterDesc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0, required};
	}
	void Backend::ObserveVSync(VSyncSubmission submission, HRESULT result)
	{
		VSyncObservation previous;
		{
			std::scoped_lock lock(vsyncMutex_);
			previous = vsyncState_;
			vsyncState_ = {true, submission, result, provider_};
		}
		if (!previous.observed || previous.submission != submission || previous.provider != provider_ || previous.result != result) {
			logger::info("[Presentation] VSync policy={} provider={} incomingSync={} incomingFlags=0x{:X} submittedSync={} submittedFlags=0x{:X} autoMetering={} dlssgRequiresOff={} result=0x{:08X}",
				VSyncName(submission.mode), provider_ == FrameGenerationProvider::XeFG ? "XeFG" : "NVIDIA",
				submission.incomingSync, submission.incomingFlags, submission.sync, submission.flags,
				submission.autoMetering, submission.autoRequiredOff, static_cast<unsigned>(result));
		}
	}
	HRESULT Backend::BeforePresent(ID3D12Resource* a_source, ID3D12Resource* a_destination, DXGI_COLOR_SPACE_TYPE a_colorSpace)
	{
		if (!Ready()) { return FAILED(fault_) ? fault_ : E_UNEXPECTED; }
		FinishScreenshot();
		auto& timer = interop_.Timer();
		timer.SetEnabled(PerformanceTuning::GetSingleton()->TimingEnabled());
		if (!timerReported_ && timer.Availability() != S_FALSE) {
			timerReported_ = true;
			if (SUCCEEDED(timer.Availability())) { logger::info("[Performance D3D12] presenting-queue stage timing ready"); }
			else {
				logger::warn("[Performance D3D12] presenting-queue stage timing unavailable (0x{:08X}); rendering continues",
					static_cast<std::uint32_t>(timer.Availability()));
			}
		}
		const auto oldReflexRequest = session_.Snapshot().reflexRequested;
		const auto oldReflexSubmitted = session_.Snapshot().reflexSubmitted;
		const auto oldFrameLimit = session_.Snapshot().frameLimitSubmittedUs;
		const auto oldGeneration = session_.Snapshot().generationRequested;
		const auto oldLimited = session_.Snapshot().generationLimited;
		const auto oldRecomposition = session_.Snapshot().options.enableUserInterfaceRecomposition;
		if (!startupTransport_ && !nativePresenterFallback_ && provider_ == FrameGenerationProvider::NVIDIA) { mfgUnlock_.Tick(); }
		session_.SetMFGUnlockState(mfgUnlock_.Snapshot().UsesCompatibilityUnlock(), mfgUnlock_.Snapshot().Ready());
		session_.RequestGeneration(GenerationConfiguration());
		session_.RequestReflexMode(ReflexConfiguration());
		session_.RequestUIRecomposition(UIRecompositionConfiguration());
		if (releaseForPreset_.exchange(false, std::memory_order_relaxed) && !startupTransport_ && !nativePresenterFallback_ && provider_ == FrameGenerationProvider::NVIDIA) {
			session_.RequestFeatureRelease();
			logger::info("[SourceDLSSG] frame generation preset {}: releasing DLSS-G for one Present to recreate it",
				PresetLabel(PresetConfiguration()));
		}
		session_.RequestOnlyGenerated(XeFGOnlyGenerated());
		session_.RequestOutputFPSLimit(UpdateFrameLimit());
		const bool prepared = nativePresenterFallback_ ? fallbackPrepared_ : provider_ == FrameGenerationProvider::XeFG ? xefg_.Snapshot().prepared :
			session_.Snapshot().stage == SessionStage::Rendering;
		if (prepared) {
			if (uiSource_ && !Check(interop_.CopyInput(uiSource_.Get(), ui_), "late UI copy")) { return fault_; }
		}
#if !defined(TRP_NO_NEURAL_RENDERING)
		const auto options = prepared ? frameNeuralOptions_ : NeuralConfiguration();
		auto unavailable = NeuralUnavailableReason(options);
		const bool eligible = prepared && neuralEligible_ && !unavailable;
		bool active = options.enabled && eligible && (!options.WorldOnly() || neuralEvaluatedEarly_);
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
				neuralTimestampFrequency_, interop_.Marks(Work::SwapChain), objectMaskCurrent_ ? objectMask_.texture12.Get() : nullptr,
                {frameConstants_.jitterOffset.x,frameConstants_.jitterOffset.y})) {
				if (!RefuseNeuralCreation(options.runtimePath)) {
					FailNeuralRecording();
					return fault_; // Retain objects; the failed recording was discarded.
				}
				// This frame presents without NR; the next one uses the next runtime.
				active = false;
				unavailable = "NR runtime refused for this GPU; switching runtime";
			} else {
				realSource = neuralPass_->Composed();
			}
		}
#endif
		const auto marks = interop_.Marks(Work::SwapChain);
		HRESULT outputResult;
		DXGI_COLOR_SPACE_TYPE outputColorSpace = a_colorSpace;
		marks.Begin(D3D12Stage::kOutput);
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
		marks.End(D3D12Stage::kOutput);
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
		// XeFG starts generating only once frames have settled after a new
		// presenter or transition; see XeFGSettleGate.
		const auto frameStart = std::chrono::steady_clock::now();
		const double frameInterval = lastHostFrame_ == std::chrono::steady_clock::time_point{} ? 0.0 :
			std::chrono::duration<double, std::milli>(frameStart - lastHostFrame_).count();
		lastHostFrame_ = frameStart;
		if (transitionBlocked || transitionWarmup != 0) { xefgSettle_.Reset(); }
		bool xefgSettled = true;
		if (provider_ == FrameGenerationProvider::XeFG && !transitionBlocked && transitionWarmup == 0 && !xefgSettle_.Settled()) {
			xefgSettled = xefgSettle_.Observe(frameInterval);
			if (xefgSettled) { logger::info("[XeFG] frames settled after {} warm-up frames; generation may start", xefgSettle_.Held()); }
		}
		const bool generationAllowed = !nativePresenterFallback_ && enabled_ && !transitionBlocked && transitionWarmup == 0 && xefgSettled;
		if (!nativePresenterFallback_ && provider_ == FrameGenerationProvider::XeFG) {
			// The same premultiplied UI layer DLSS-G recomposes. The HDR targets are
			// current only when HDR output composed this prepared frame; otherwise
			// XeFG holds generation rather than interpolate an older world image.
			const auto destination = a_destination->GetDesc();
			const auto layers = SelectXeFGLayers(destination.Format, hudless_.desc.Format,
				hdrOutputPass_ != nullptr, prepared && hdrLayersComposed, uiSource_ != nullptr);
			auto pick = [&](XeFGLayer layer, ID3D12Resource* hdr, ID3D12Resource* standard) {
				return layer == XeFGLayer::Converted ? hdr : layer == XeFGLayer::Standard ? standard : nullptr;
			};
			auto* intelHudless = pick(layers.hudless, hdrOutputPass_ ? hdrOutputPass_->HudlessTarget() : nullptr, hudless_.texture12.Get());
			auto* intelUI = pick(layers.ui, hdrOutputPass_ ? hdrOutputPass_->UITarget() : nullptr, ui_.texture12.Get());
			// No FP16/scRGB reinterpretation. XeFG copies matching native-format layers.
			const bool compatible = intelHudless && intelHudless->GetDesc().Format == destination.Format;
			if (intelUI) {
				const auto ui = intelUI->GetDesc();
				if (ui.Format != destination.Format || ui.Width != destination.Width || ui.Height != destination.Height) { intelUI = nullptr; }
			}
			if (!CheckXeFG(xefg_.SetDebugView(XeFGOnlyGenerated(), XeFGTagGenerated()))) { return fault_; }
			const bool meter = XeFGFlipMetering() && VSyncConfigurationMode() != VSyncMode::On;
			if (xefg_.FlipMeteringRequested() != meter) xefg_.SetFlipMetering(meter);
			xefg_.SetSceneChangeThreshold(XeFGSceneChangeThresholdConfiguration());
			if (enabled_ && (!generationAllowed || !prepared || !compatible) && xefg_.Snapshot().presents % 600 == 0) {
				logger::info("[XeFG] generation held gate={} prepared={} compatible={} sourceFormat={} outputFormat={} convertedThisFrame={}",
					generationAllowed, prepared, compatible, static_cast<unsigned>(hudless_.desc.Format),
					static_cast<unsigned>(destination.Format), hdrLayersComposed);
			}
			marks.Begin(D3D12Stage::kXeFGInputs);
			if (!CheckXeFG(xefg_.BeforePresent(list, motion_.texture12.Get(), depth_.texture12.Get(), intelHudless, intelUI,
				generationAllowed && prepared && compatible, UIRecompositionConfiguration(),
				frameOutputLimit_, XeFGConfiguration().generatedFrames))) { return fault_; }
			marks.End(D3D12Stage::kXeFGInputs);
		}
		xefgFrameId_ = xefg_.Snapshot().frameId;
		xefgFrameModel_ = NRModelActivity::Unknown;
		xefgFrameAlternate_ = false;
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (active && neuralPass_) {
			xefgFrameModel_ = neuralPass_->ModelActivity();
			xefgFrameAlternate_ = NeuralRendering::EffectiveModelRate(options.reconstruction.modelRate) == NeuralRendering::ModelRate::EveryOtherFrame;
		}
#endif
		if (!Check(outputResult, "record native output copy/conversion") ||
			!Check(interop_.Submit(Work::SwapChain), "submit native output copy")) { return fault_; }
		xefgSubmitTime_ = std::chrono::steady_clock::now();
		xefgBeatPaced_ = false; xefgBeatHoldMs_ = 0;
		if (provider_ == FrameGenerationProvider::XeFG && XeFGModelBeatRequested() &&
			!XeFGModelBeatUnavailable() && xefgFrameAlternate_ && xefg_.Snapshot().enabled &&
			xefgFrameModel_ != NRModelActivity::Unknown && !frameNeuralReset_ && !frameNeuralModelChanged_) {
			const auto decision = xefgModelBeat_.Plan(
				std::chrono::duration<double, std::micro>(xefgSubmitTime_.time_since_epoch()).count(), xefgFrameModel_);
			xefgBeatPaced_ = decision.paced;
			if (decision.deadlineUs > 0) {
				std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
					std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::micro>(decision.deadlineUs))));
			}
			const auto released = std::chrono::steady_clock::now();
			xefgBeatHoldMs_ = std::chrono::duration<double, std::milli>(released - xefgSubmitTime_).count();
			xefgModelBeat_.Released(std::chrono::duration<double, std::micro>(released.time_since_epoch()).count());
		} else xefgModelBeat_.Reset();
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
		if (!startupTransport_ && !nativePresenterFallback_ && provider_ == FrameGenerationProvider::NVIDIA && !nvidiaNeedsPresent_ && ((prepared && !CheckSession(session_.CompleteInputWrites())) ||
			!CheckSession(session_.BeforePresent(generationAllowed)))) { return fault_; }
		if (!nativePresenterFallback_ && provider_ == FrameGenerationProvider::XeFG && !CheckXeFG(xefg_.FinalizePresent())) { return fault_; }
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
			// A new pass runs from the frame after its feature's creation.
			neuralSnapshot_.effectivePasses = active && neuralPass_ ?
				(std::max)(1, (std::min)(options.EffectivePasses(), neuralPass_->ReadyPasses())) : options.EffectivePasses();
			neuralSnapshot_.passOverride = active ? options.passOverride : NeuralRendering::PassOverride::None;
			neuralSnapshot_.passNote = active && neuralPass_ ? neuralPass_->PassNote() : std::string{};
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
		neuralFrameBegun_ = neuralEvaluatedEarly_ = frameNeuralModelChanged_ = false;
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
		if (startupTransport_) {
			if (!Check(interop_.WaitForInputReaders(nullptr, 0), "normal post-Present fence bridge") ||
				!Check(interop_.Drain(), "normal presentation retirement")) { return fault_; }
			++transportFrame_;
			neuralFrameBegun_ = neuralEvaluatedEarly_ = frameNeuralModelChanged_ = false;
			return a_result;
		}
		if (nativePresenterFallback_) {
			fallbackPrepared_ = false;
			++replacementFrame_;
			PollDisplayHDR(false);
			// The native presenter has no SDK input reader/latency owner. Retire
			// output/NR work before the next D3D11 producer reuses shared textures.
			return Check(interop_.WaitForInputReaders(nullptr, 0), "native fallback queue bridge") &&
				Check(interop_.Drain(), "native fallback input retirement") ? a_result : fault_;
		}
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
			xefgFrameReuseMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - reuseStart).count();
			xefgReuseMs_.push_back(static_cast<float>(xefgFrameReuseMs_));
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
		{
			// Frame generation's cost depends on these. A change restarts the
			// rolling timing window so each configuration's percentiles stand alone.
			const bool active = GenerationActive();
			const bool dynamic = provider_ == FrameGenerationProvider::NVIDIA && state.options.mode == sl::DLSSGMode::eDynamic;
			const auto key = static_cast<std::uint64_t>(provider_) | static_cast<std::uint64_t>(PresetConfiguration()) << 4 |
				static_cast<std::uint64_t>(active ? EffectiveMultiplier() : 0) << 8 | std::uint64_t{dynamic} << 16 |
				std::uint64_t{NvidiaOverlapNextFrame()} << 17;
			if (key != frameGenerationTimingKey_) {
				frameGenerationTimingKey_ = key;
				if (auto* timing = PerformanceTuning::GetSingleton(); timing->TimingEnabled()) {
					timing->ResetTimingWindow();
					logger::info("[Performance] frame generation {} {} preset={} overlap={}; timing window restarted",
						ProviderName(provider_), active ? std::format("x{}{}", EffectiveMultiplier(), dynamic ? " dynamic" : "") : "off",
						PresetLabel(PresetConfiguration()), NvidiaOverlapNextFrame());
				}
			}
		}
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
	bool Backend::Quiesce(RetirementPurpose purpose)
	{
		xefgModelBeat_.Reset(); xefgBeatPaced_ = false;
		enabled_ = false;
		ResetPresentationFeedback();
		if (FAILED(fault_)) { return false; } // Never destroy a failed, unsubmitted NR recording.
		if (!ready_) { return SUCCEEDED(interop_.Drain()); }
		if (!nativePresenterFallback_ && !presenterReplacement_.Pending() && provider_ == FrameGenerationProvider::XeFG) {
			if (!CheckXeFG(xefg_.Disable())) { return false; }
		} else if (!nativePresenterFallback_ && !presenterReplacement_.Pending() &&
			session_.Snapshot().stage != SessionStage::Stopped && !CheckSession(session_.Stop())) { return false; }
		bool retainedNeural = false;
#if !defined(TRP_NO_NEURAL_RENDERING)
		retainedNeural = purpose == RetirementPurpose::UpscalerReplacement && neuralPass_;
#endif
		const auto retired = RetireSharedResources(purpose, [&] {
			return Check(interop_.SignalD3D11(Work::SwapChain), "last D3D11 work before host retirement") &&
				Check(interop_.Drain(), "retire shared host work");
		}, [&] {
#if !defined(TRP_NO_NEURAL_RENDERING)
		if (neuralPass_) {
			neuralPass_->RetireTelemetry();
			std::scoped_lock lock(neuralMutex_);
			neuralSnapshot_.telemetry = neuralPass_->Telemetry();
		}
#endif
		}, [&] {
		ReleaseGuides();
#if !defined(TRP_NO_NEURAL_RENDERING)
		neuralPass_.reset(); // The presenting queue and NVIDIA input readers are retired.
#endif
		hdrPass_.reset();
		hdrOutputPass_.reset();
		}, [&] {
		hdrFrameTagged_ = false;
		neuralHistory_.Invalidate();
		neuralEligible_ = false;
		neuralFrameBegun_ = neuralEvaluatedEarly_ = frameNeuralModelChanged_ = false;
		{
			std::scoped_lock lock(neuralMutex_);
			neuralSnapshot_.active = false;
			neuralSnapshot_.status = purpose == RetirementPurpose::Resize ?
				"NR retired for resize; next eligible frame recreates the feature" :
				"NR retained for fixed-size upscaler replacement; next eligible frame resets history";
		}
		});
		if (retired) {
			logger::info("[SourceHostRetirement] purpose={} resourcesRetained={} neuralFeatureRetained={}",
				purpose == RetirementPurpose::Resize ? "resize" : "upscaler", purpose != RetirementPurpose::Resize, retainedNeural);
		}
		return retired;
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
		const bool hdr = HDRSignalled();
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
		if (startupTransport_ || nativePresenterFallback_) { return Ready(); }
		if (nvidiaNeedsPresent_) { return Ready(); }
		if (provider_ == FrameGenerationProvider::XeFG) { xefg_.ResetHistory(); return Ready(); }
		return Ready() && CheckSession(session_.ResumeAfterResize());
	}
	bool Backend::ResumeAfterUpscalerReplacement()
	{
		if (!ResumeAfterResize()) { return false; }
		// Stop submitted Off without a real Present. Keep it Off while the new
		// algorithm republishes reset guides; only successful real Presents count.
		// Reuse the existing transition gate rather than discard option changes.
		if (!TransitionBlocked()) {
			transitionWarmupPresents_.store(TransitionWarmupPresents, std::memory_order_release);
		}
		logger::info("[SourceUpscaler] resumed with generation gated for three real presents");
		return true;
	}

	bool Backend::PreflightProviderSwitch()
	{
		if (nativePresenterFallback_ || presenterReplacement_.Pending()) { return false; }
		// Normalize before transition/camera gates, including a later Apply of a
		// saved NVIDIA default on unsupported hardware. No INI is written here.
		RequestProvider(RequestedProvider());
		if (nvidiaNeedsPresent_) { return false; }
		const auto request = RequestedProvider();
		if ((request == provider_ && !recreateXeFG_) || !cameraAvailable_ || TransitionBlocked()) { return false; }
		if (request == FrameGenerationProvider::NVIDIA && !NvidiaFrameGenerationAvailable()) {
			providerStatus_ = "NVIDIA frame generation unavailable; Intel presenter retained";
			RequestProvider(provider_); return false;
		}
		if (request == FrameGenerationProvider::NVIDIA && NvidiaSwitchBlocked()) {
			providerStatus_ = "NVIDIA switch needs a restart (NR started first); Intel presenter retained";
			logger::warn("[FrameGeneration] NVIDIA frame generation needs a restart: NR started NVIDIA NGX before "
				"Streamline in this XeSS session. Intel presenter retained; save NVIDIA and restart, or switch before enabling NR.");
			RequestProvider(provider_); return false;
		}
		if (request == FrameGenerationProvider::XeFG) {
			const bool compatible = XeFGPresenter::SupportsFormat(presenterDesc_.BufferDesc.Format) &&
				hudless_.texture12 && XeFGPresenter::SupportsFormat(hudless_.desc.Format);
			const auto result = compatible ? xefg_.Probe(device12_.Get(), PluginPaths::IntelDirectory(), xefgTensorRequested_) : DXGI_ERROR_UNSUPPORTED;
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
	int Backend::UpdateFrameLimit()
	{
		const auto mode = static_cast<FrameLimitMode>(frameLimit_.load(std::memory_order_relaxed));
		const auto now = std::chrono::steady_clock::now();
		const bool measured = steadyLast_.time_since_epoch().count() != 0;
		const double intervalUs = measured ? std::chrono::duration<double, std::micro>(now - steadyLast_).count() : 0;
		steadyLast_ = now;
		if (mode != FrameLimitMode::Steady) {
			steadyPacing_.Reset();
			steadyRenderedFPS_.store(0, std::memory_order_relaxed);
			return frameOutputLimit_ = mode == FrameLimitMode::Fixed ? outputFPSLimit_.load(std::memory_order_relaxed) : 0;
		}
		if (measured && steadyPacing_.Observe(intervalUs)) {
			logger::info("[FrameLimit] steady cap {:.1f} rendered FPS", steadyPacing_.TargetFPS());
		}
		steadyRenderedFPS_.store(static_cast<float>(steadyPacing_.TargetFPS()), std::memory_order_relaxed);
		// The limiters cap output, so scale the rendered cap by this frame's multiplier.
		const unsigned outputs = provider_ == FrameGenerationProvider::XeFG ?
			(xefg_.Snapshot().enabled ? xefg_.Snapshot().generatedFrames + 1 : 1u) :
			(GenerationEnabled(session_.Snapshot().options.mode) ? session_.Snapshot().options.numFramesToGenerate + 1 : 1u);
		return frameOutputLimit_ = SteadyOutputLimit(steadyPacing_.TargetUs(), outputs);
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
				frameConstants_.depthInverted == sl::eTrue, result, replacementFrame_, XeFGConfiguration(),
				IntelPipelines(XeFGPipelineCache(), xefg_.PipelineCacheName(), xefg_.RuntimeModule()), replacementNative_.Get()) :
			streamlineFactory_->CreateSwapChain(queue_.Get(), &presenterDesc_, result);
	}
	HRESULT Backend::ReleasePresenter(SwapChain& wrapper)
	{
		// Probe the actual DXGI object, never Intel's proxy: a retained proxy
		// makes xefgSwapChainDestroy fail even when no external owner exists.
		auto* native = xefg_.NativeSwapChain();
		if (!native && (nativePresenterFallback_ || provider_ == FrameGenerationProvider::NVIDIA)) { native = retainedNative_.Get(); }
		PresenterReleaseProbe probe(native);
		logger::info("[Presenter release] before hwnd={} windowed={} describe=0x{:08X} fullscreenQuery=0x{:08X} native={} buffers={} "
			"wrapperInterfaces={}/{}/{}/{}/{} wrapperBuffers={}/{} retainedProxy={} sdk={} xell={} hdrPass={} hdrOutput={} reuseNative={}",
			static_cast<void*>(probe.desc.OutputWindow), probe.windowed, static_cast<unsigned>(probe.described), static_cast<unsigned>(probe.fullscreenResult),
			static_cast<void*>(native), probe.desc.BufferCount, bool(wrapper.inner_), bool(wrapper.inner1_), bool(wrapper.inner2_), bool(wrapper.inner3_), bool(wrapper.inner4_),
			bool(wrapper.nativeBuffers_[0]), bool(wrapper.nativeBuffers_[1]), bool(retainedNative_), xefg_.HasContext(), xefg_.HasLatencyContext(),
			hdrPass_ ? hdrPass_->FrameReferenceCount() : 0, hdrOutputPass_ ? hdrOutputPass_->FrameReferenceCount() : 0, bool(replacementNative_));
		const auto released = ReleasePresenterAfterRetirement([&] {
			// The facade textures are independent shared allocations, not native
			// swapchain buffers. Keep their game-visible identities and views.
			if (hdrOutputPass_) { hdrOutputPass_->ReleaseFrameReferencesAfterRetirement(); }
			if (hdrPass_) { hdrPass_->ReleaseFrameReferencesAfterRetirement(); }
			wrapper.ReleaseInnerAfterRetirement();
			retainedNative_.Reset();
			hdrFrameTagged_ = false;
		}, [&] { return xefg_.Destroy(); }, [&] {
			// DXGI flip-model replacement requires deferred D3D11 destruction to
			// complete after *all* references (including the SDK's) are released.
			context11_->ClearState();
			context11_->Flush();
		}, [&] {
			if (!Check(interop_.SignalD3D11(Work::SwapChain), "flush deferred presenter destruction") ||
				!Check(interop_.WaitForInputReaders(nullptr, 0), "released presenter queue bridge") ||
				!Check(interop_.Drain(), "drain released presenter")) { return fault_; }
			return S_OK;
		});
		probe.Release([&](UINT i, void* identity, HRESULT hr, ULONG count) {
			logger::info("[Presenter release] buffer={} native={} probe=0x{:08X} remaining={}", i, identity, static_cast<unsigned>(hr), count);
		}, [&](void* identity, ULONG count) {
			logger::info("[Presenter release] after native={} remaining={} release=0x{:08X} reuseNative={} "
				"wrapperInterfaces={}/{}/{}/{}/{} wrapperBuffers={}/{} retainedProxy={} sdk={} xell={} hdrPass={} hdrOutput={}", identity, count, static_cast<unsigned>(released),
				bool(replacementNative_), bool(wrapper.inner_), bool(wrapper.inner1_), bool(wrapper.inner2_), bool(wrapper.inner3_), bool(wrapper.inner4_),
				bool(wrapper.nativeBuffers_[0]), bool(wrapper.nativeBuffers_[1]), bool(retainedNative_), xefg_.HasContext(), xefg_.HasLatencyContext(),
				hdrPass_ ? hdrPass_->FrameReferenceCount() : 0, hdrOutputPass_ ? hdrOutputPass_->FrameReferenceCount() : 0);
		});
		// Probes themselves postpone final native destruction. Flush/drain once
		// more after their last Release, before another HWND creation is allowed.
		if (FAILED(released)) { return released; }
		context11_->ClearState(); context11_->Flush();
		if (!Check(interop_.WaitForInputReaders(nullptr, 0), "post-probe queue bridge") ||
			!Check(interop_.Drain(), "post-probe presenter retirement")) { return fault_; }
		return S_OK;
	}
	bool Backend::ApplyProviderSwitch(SwapChain& wrapper)
	{
		if (!Ready()) { return false; }
		if (startupTransport_) { return true; } // Partial startup ownership is retained until restart.
		if (!presenterReplacement_.Pending()) {
			if (!providerSwitchPending_) {
				return nativePresenterFallback_ || provider_ != FrameGenerationProvider::XeFG || CheckXeFG(xefg_.BeginFrame());
			}
			replacementProvider_ = RequestedProvider();
			replacementFrame_ = FrameIndex();
			logger::info("[FrameGeneration] replacing presenter current={} requested={} depth={}->{} mfgRecreate={} tensorRecreate={}",
				ProviderName(provider_), ProviderName(replacementProvider_), xefg_.Snapshot().invertedDepth,
				frameConstants_.depthInverted == sl::eTrue, XeFGNeedsRecreation(xefg_.Snapshot().experimentalMFG, XeFGConfiguration()),
				xefg_.ProbedTensorRequest() != xefgTensorRequested_.load());
			if (provider_ == FrameGenerationProvider::XeFG && !CheckXeFG(xefg_.Disable())) { return false; }
			if (!Check(interop_.SignalD3D11(Work::SwapChain), "last producer work before provider change") ||
				!Check(interop_.WaitForInputReaders(nullptr, 0), "provider queue retirement bridge") ||
				!Check(interop_.Drain(), "retire shared work before provider change")) { return false; }
			// The factory reuses the retired native transport while Intel replaces
			// its proxy, instead of asking DXGI for a second flip chain on HWND.
			if (provider_ == FrameGenerationProvider::XeFG && replacementProvider_ == FrameGenerationProvider::XeFG) {
				replacementNative_ = xefg_.NativeSwapChain();
				if (!replacementNative_) { return Check(E_UNEXPECTED, "missing native XeFG rebuild owner"); }
			}
			if (!Check(ReleasePresenter(wrapper), "release old presenter and SDK ownership")) { return false; }
			presenterReplacement_.Start();
		}
		const auto result = presenterReplacement_.Advance([&](bool native) {
			ComPtr<IDXGISwapChain> replacement;
			HRESULT created;
			if (native && replacementNative_) {
				// Intel can change the physical buffer count. Restore the facade's
				// two-buffer native contract only after failed SDK owners retire.
				created = PresenterFactory::NativeFallbackAfterRetirement(replacementNative_.Get(), queue_.Get());
				if (SUCCEEDED(created)) { replacement = replacementNative_; }
			} else {
				created = native ? nativeFactory_->CreateSwapChain(queue_.Get(), &presenterDesc_, &replacement) :
					CreatePresenter(replacementProvider_, &replacement);
			}
			// Retain partial creation until the discard callback proves retirement.
			retainedNative_ = replacement;
			if (SUCCEEDED(created)) { created = wrapper.AttachInner(replacement.Get()); }
			if (FAILED(created)) {
				logger::warn("[FrameGeneration] {} presenter creation attempt={} failed HRESULT=0x{:08X}; {}",
					native ? "native fallback" : ProviderName(replacementProvider_), presenterReplacement_.Attempts(),
					static_cast<unsigned>(created), xefg_.Status());
			}
			// A failed host/vendor initialization may retain unpublished GPU owners.
			// Do not let a generic E_FAIL authorize the discard/retry callbacks.
			return Ready() ? created : E_UNEXPECTED;
		}, [&] { return ReleasePresenter(wrapper); });
		if (!Ready()) { return false; } // A vendor initialization/queue fault keeps its retained-owner boundary.
		if (result == S_FALSE) {
			providerStatus_ = "Presenter replacement waiting for DXGI; retry on the next frame";
			return true; // The facade remains valid while no native chain exists.
		}
		if (!Check(result, "presenter recovery exhausted or retirement failed")) { return false; }
		nativePresenterFallback_ = presenterReplacement_.Native();
		replacementNative_.Reset();
		provider_ = replacementProvider_; recreateXeFG_ = providerSwitchPending_ = false;
		if (nativePresenterFallback_) { RequestProvider(provider_); }
		presenterPresents_ = 0; presenterReplaced_ = true;
		ResetPresentationFeedback();
		transitionWarmupPresents_.store(TransitionWarmupPresents);
		neuralHistory_.Invalidate();
		hdrColorSpaceApplied_ = false; ApplyNativeColorSpace();
		nvidiaNeedsPresent_ = !startupTransport_ && !nativePresenterFallback_ && provider_ == FrameGenerationProvider::NVIDIA;
		providerStatus_ = nativePresenterFallback_ ? "Native presentation restored; frame generation unavailable until restart" :
			std::format("{} presenter; latency owner={}", ProviderName(provider_), provider_ == FrameGenerationProvider::XeFG ? "XeLL" : "Reflex");
		if (nvidiaNeedsPresent_) { providerStatus_ = "NVIDIA replacement waiting for first Present; Reflex suspended"; }
		logger::info("[FrameGeneration] provider change complete {}; shared DLSS/NR resources retained", providerStatus_);
		return true;
	}
	ID3D12PipelineLibrary* Backend::IntelPipelines(PipelineCache& cache, const char* name, HMODULE runtime)
	{
		if (!cache.Opened()) {
			cache.Open(device12_.Get(), PipelineCacheFile(name, adapterVendor_, adapterDevice_, adapterDriver_, runtime));
			logger::info("[PipelineCache] {} {}", name, cache.Status());
		}
		return cache.Library();
	}
	const char* Backend::XeFGModelBeatUnavailable() const
	{
		if (provider_ != FrameGenerationProvider::XeFG) return "Select Intel XeFG.";
		if (!ready_ || !enabled_) return "Enable frame generation.";
		if (xefg_.TensorInUse()) return "Requires plain XeFG; turn off the tensor model and Apply.";
		if (xefg_.Snapshot().generatedFrames != 1) return "This experiment is for XeFG x2 only.";
		if (XeFGGpuInputWait()) return "Turn off Input reuse: GPU fence wait for this experiment.";
		if (static_cast<FrameLimitMode>(frameLimit_.load(std::memory_order_relaxed)) != FrameLimitMode::Off)
			return "Set Frame limit to Off for this experiment.";
#if !defined(TRP_NO_NEURAL_RENDERING)
		const auto options = NeuralConfiguration();
		if (!options.enabled) return "Enable Neural Rendering and Apply.";
		if (NeuralRendering::EffectiveModelRate(options.reconstruction.modelRate) != NeuralRendering::ModelRate::EveryOtherFrame)
			return "Select Model rate: Every other frame and Apply.";
		if (options.compare.freeze) return "Turn off Freeze frame.";
		{ std::scoped_lock lock(neuralMutex_);
			if (!neuralSnapshot_.active) return "Requires NR running in the world.";
		}
		return nullptr;
#else
		return "Neural Rendering is unavailable in this build.";
#endif
	}
	void Backend::ObservePresentTiming(double a_prepareMs, double a_presentMs, double a_finishMs, double hostStartUs, double innerStartUs)
	{
		++presenterPresents_;
		if (provider_ == FrameGenerationProvider::XeFG && !xefg_.TensorInUse() && !presenterReplaced_) {
			// Correlate actual model activity with this host frame; never infer it
			// from frame parity or the (delayed) telemetry query results. Bound log
			// work to 48 consecutive frames per 600, also with the experiment off.
			const double interval = xefgPreviousHostUs_ ? (hostStartUs - xefgPreviousHostUs_) / 1000 : 0;
			const double innerInterval = xefgPreviousInnerUs_ ? (innerStartUs - xefgPreviousInnerUs_) / 1000 : 0;
			xefgPreviousHostUs_ = hostStartUs; xefgPreviousInnerUs_ = innerStartUs;
			if (!xefgFrameAlternate_ || xefgTraceRequested_ != XeFGModelBeatRequested()) xefgTraceFrames_ = 0;
			xefgTraceRequested_ = XeFGModelBeatRequested();
			if (xefgFrameAlternate_ && xefgTraceFrames_++ % 600 < 48) {
				const auto& s = xefg_.Snapshot();
				const auto vsync = VSyncState();
				logger::info("[XeFG beat] epoch={} frame={} model={} hostDtMs={:.3f} innerDtMs={:.3f} innerPresentMs={:.3f} reuse={} reuseCpuMs={:.3f} "
					"submitUs={} holdMs={:.3f} targetMs={:.3f} phaseSkewMs={:.3f} pairSamples={} requested={} paced={} xellSleepMs={:.3f} outputs={} "
					"vsync={} incomingSync={} submittedSync={} flags=0x{:X} batchStart=unavailable outputSpacing=unavailable",
					s.epoch, xefgFrameId_, ModelActivityName(xefgFrameModel_), interval, innerInterval, a_presentMs,
					XeFGGpuInputWait() ? "gpu-fence" : "cpu-drain", xefgFrameReuseMs_,
					std::chrono::duration_cast<std::chrono::microseconds>(xefgSubmitTime_.time_since_epoch()).count(),
					xefgBeatHoldMs_, xefgModelBeat_.TargetUs() / 1000, xefgModelBeat_.PhaseDifferenceUs() / 1000,
					xefgModelBeat_.PairSamples(), XeFGModelBeatRequested(), xefgBeatPaced_, s.sleepMs,
					s.framesPresented, VSyncName(vsync.submission.mode), vsync.submission.incomingSync,
					vsync.submission.sync, vsync.submission.flags);
			}
		} else { xefgPreviousHostUs_ = xefgPreviousInnerUs_ = 0; xefgTraceFrames_ = 0; }
		// Intel stores pipelines as it first uses them; persist what has grown.
		if (presenterPresents_ % 600 == 0) {
			for (auto* cache : {&xefgPipelines_, &xefgTensorPipelines_, &xessPipelines_}) {
				const auto before = cache->Status();
				cache->SaveIfGrown();
				if (cache->Status() != before) {
					logger::info("[PipelineCache] {} {}", cache == &xefgPipelines_ ? "xefg" : cache == &xefgTensorPipelines_ ? "xefg-tensor" : "xess",
						cache->Status());
				}
			}
		}
		const bool replaced = std::exchange(presenterReplaced_, false);
		const double total = a_prepareMs + a_presentMs + a_finishMs;
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (!slowPresent_.Admit(total, now)) { return; }
		const bool intel = provider_ == FrameGenerationProvider::XeFG;
		const auto& state = xefg_.Snapshot();
		logger::warn("[Present] slow host Present total={:.1f} ms prepare={:.1f} present={:.1f} finish={:.1f} provider={} "
			"presenterEpoch={} presentsSinceCreation={} presenterReplaced={} generation={} multiplier=x{} xellSleepMs={:.1f} "
			"suppressedSinceLast={}",
			total, a_prepareMs, a_presentMs, a_finishMs, ProviderName(provider_),
			intel ? state.epoch : session_.Snapshot().presentationEpoch, presenterPresents_, replaced,
			GenerationActive(), EffectiveMultiplier(), intel ? state.sleepMs : 0.0f, slowPresent_.TakeSuppressed());
	}
	bool Backend::NvidiaSwitchBlocked() const
	{
#if !defined(TRP_NO_NEURAL_RENDERING)
		return NvidiaSwitchNeedsRestart(nvidiaInitialized_, NeuralRendering::PublicNGXInitializedInProcess());
#else
		return false;
#endif
	}
}
