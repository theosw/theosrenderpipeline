#include "NeuralRenderingFeatureSession.h"

#include "NeuralRenderingCreationContext.h"
#include "NeuralRenderingModulePathHook.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "NeuralRuntimeKernels.h"
#include "NeuralRuntimeCompatibility.h"
#include "NeuralRenderingSubrect.h"

#include <PCH.h>

#include <nvsdk_ngx.h>

#include <array>
#include <chrono>
#include <atomic>
#include <cmath>
#include <format>
#include <limits>
#include <mutex>
#include <utility>

#include <wrl/client.h>

namespace TheosRenderPipeline::NeuralRendering
{
	namespace
	{
		constexpr unsigned long long kTheosRenderPipelineGenericCmsId = 0x0876232Cull;
		constexpr auto kFeatureApiVersion = static_cast<NVSDK_NGX_Version>(0x15);
		constexpr const char* kTheosRenderPipelineNGXProjectId = "f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11";
		constexpr const char* kTheosRenderPipelineNGXEngineVersion = "0.1.0-neural-rendering-source";
		constexpr auto kNeuralRenderingFeature = static_cast<NVSDK_NGX_Feature>(0x12);
		std::atomic<bool> publicNGXInitialized{};
		// Covers first load, kernel publication, public NGX and direct feature init.
		// Subsequent passes must not observe a partly initialized shared module.
		std::mutex initializationMutex;

		using D3D12Init = NVSDK_NGX_Result(NVSDK_CONV*)(
			unsigned long long,
			const wchar_t*,
			ID3D12Device*,
			NVSDK_NGX_Version);
		using D3D12InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(
			unsigned long long,
			const wchar_t*,
			ID3D12Device*,
			NVSDK_NGX_Version,
			const NVSDK_NGX_Parameter*);
		using D3D12GetCapabilityParameters =
			NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
		using D3D12DestroyParameters =
			NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using D3D12CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
			ID3D12GraphicsCommandList*,
			NVSDK_NGX_Feature,
			NVSDK_NGX_Parameter*,
			NVSDK_NGX_Handle**);
		using D3D12EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
			ID3D12GraphicsCommandList*,
			const NVSDK_NGX_Handle*,
			const NVSDK_NGX_Parameter*,
			PFN_NVSDK_NGX_ProgressCallback_C);
		using D3D12ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

		bool IsUsableInitResult(NVSDK_NGX_Result a_result)
		{
			return NVSDK_NGX_SUCCEED(a_result) ||
				a_result == NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;
		}

		std::filesystem::path GetDataPath()
		{
			std::error_code error;
			auto path = std::filesystem::temp_directory_path(error);
			if (error) {
				return {};
			}
			path /= L"TheosRenderPipeline";
			path /= L"NeuralRenderingFeatureSession";
			std::filesystem::create_directories(path, error);
			return error ? std::filesystem::path{} : path;
		}

		HMODULE FindNormalLoader()
		{
			if (auto module = ::GetModuleHandleW(L"nvngx.dll")) {
				return module;
			}
			return ::GetModuleHandleW(L"_nvngx.dll");
		}

	}

	struct FeatureSession::State
	{
		std::unique_ptr<CreationContext> creationContext;
		HMODULE module{ nullptr };
		bool ownsModule{ false };
		std::unique_ptr<ModulePathHook> modulePathHook;
		NVSDK_NGX_Parameter* parameters{ nullptr };
		D3D12DestroyParameters destroyParameters{ nullptr };
		D3D12CreateFeature createFeature{ nullptr };
		D3D12EvaluateFeature evaluateFeature{ nullptr };
		D3D12ReleaseFeature releaseFeature{ nullptr };
		NVSDK_NGX_Handle* feature{ nullptr };
		ID3D12Device* device{ nullptr };
		std::filesystem::path runtimePath;
		std::filesystem::path boundRuntimeRequest;
		std::uint32_t displayWidth{ 0 };
		std::uint32_t displayHeight{ 0 };
		std::uint32_t renderWidth{ 0 };
		std::uint32_t renderHeight{ 0 };
		FeatureContract contract{};
		std::uint64_t evaluationsRecorded{ 0 };
		std::uint32_t lastInitResult{ (std::numeric_limits<std::uint32_t>::max)() };
		std::uint32_t lastCreateResult{ (std::numeric_limits<std::uint32_t>::max)() };
		std::uint32_t lastEvaluateResult{ (std::numeric_limits<std::uint32_t>::max)() };
		bool attempted{ false };
		bool initialized{ false };
		bool runtimeRefused{ false };
		std::string status{ "not initialized" };

		~State()
		{
			bool released = true;
			if (feature && releaseFeature) {
				const auto result = releaseFeature(feature);
				released = NVSDK_NGX_SUCCEED(result);
				if (!released) { logger::error("[DLSSNR Source] ReleaseFeature failed (0x{:08X}); keeping the runtime module loaded", static_cast<std::uint32_t>(result)); }
			}
			if (parameters && destroyParameters) {
				const auto result = destroyParameters(parameters);
				if (NVSDK_NGX_FAILED(result)) { logger::warn("[DLSSNR Source] DestroyParameters failed (0x{:08X})", static_cast<std::uint32_t>(result)); }
			}
			modulePathHook.reset();
			// Unloading the runtime while a feature it failed to release may still
			// run would leave its code unmapped; keep it for the process lifetime.
			if (module && ownsModule && released) {
				::FreeLibrary(module);
			}
		}
	};

	void FeatureSession::StateDeleter::operator()(State* state) const
	{
		if (!DestroyAfterCreation(state)) {
			logger::error("[DLSSNR Source] retaining failed feature session: creation GPU completion is unconfirmed; relaunch required");
		}
	}

	FeatureSession::FeatureSession() : state_(new State) {}
	FeatureSession::~FeatureSession() = default;

	bool FeatureSession::EnsureInitialized(const CreateInfo& a_info)
	{
		using namespace TheosRenderPipeline::NeuralRenderingRuntimeIdentity;
		auto& state = *state_;
		if (state.initialized) {
			const bool compatible = state.device == a_info.device &&
				(!UsesReconstructionContract(state.contract.build) || a_info.allowReconstructionRuntime) &&
				state.contract == MakeFeatureContract(state.contract.build, a_info.displayWidth,
					a_info.displayHeight, a_info.renderWidth, a_info.renderHeight, a_info.networkPreset) &&
				// Bind the verified absolute request to this loaded feature, including
				// MO2 virtual paths. Resolve changed spellings and relative requests;
				// relative paths must still follow the current working directory.
				((a_info.runtimePath.is_absolute() && state.boundRuntimeRequest == a_info.runtimePath) ||
					EqualPath(state.runtimePath, a_info.runtimePath));
			if (!compatible) {
				state.status = "source session device, dimensions, preset, or runtime contract changed";
			}
			return compatible;
		}
		if (state.attempted) {
			return false;
		}
		state.attempted = true;
		std::scoped_lock initializationLock(initializationMutex);
		const auto started = std::chrono::steady_clock::now();
		const auto elapsed = [&] {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		};
		if (!a_info.device || a_info.runtimePath.empty() || a_info.displayWidth == 0 ||
			a_info.displayHeight == 0 || a_info.renderWidth == 0 || a_info.renderHeight == 0) {
			state.status = "source session inputs are incomplete";
			return false;
		}

		const auto identity = VerifySource(a_info.runtimePath, a_info.allowReconstructionRuntime);
		const double verified = elapsed();
		// Failures before public NGX or the runtime's own init ran no vendor GPU work:
		// refuse this runtime so the backend can fall back instead of stopping.
		if (!identity.matched) {
			state.status = "NR runtime is not a supported build (file version 310 or later, or a known runtime)";
			state.runtimeRefused = true;
			return false;
		}
		state.contract = MakeFeatureContract(MatchRuntime(identity.size, identity.sha256, identity.version, a_info.allowReconstructionRuntime),
			a_info.displayWidth, a_info.displayHeight, a_info.renderWidth, a_info.renderHeight, a_info.networkPreset);
		if (!state.contract.Valid()) {
			state.status = "source session feature contract is invalid";
			return false;
		}
		logger::info("[DLSSNR Source] runtime build={} version={} size={} sha256={} path={}",
			RuntimeName(state.contract.build), identity.version, identity.size, identity.sha256, identity.path.string());
		if (const auto existing = ::GetModuleHandleW(L"nvngx_dlssnr.dll")) {
			if (!EqualPath(ModulePath(existing), identity.path)) {
				state.status = "a different nvngx_dlssnr.dll is already resident";
				state.runtimeRefused = true;
				return false;
			}
		}

		// Own the runtime before public NGX can discover it. When needed, install the
		// pack before either public NGX or the runtime's own init is entered.
		// MO2's virtual path is not a reliable GetModuleHandle filename lookup.
		const auto existingModule = PluginPaths::RetainLoadedModule(identity.path);
		const bool resident = existingModule != nullptr;
		state.module = existingModule ? existingModule : ::LoadLibraryExW(identity.path.c_str(), nullptr,
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		state.ownsModule = state.module != nullptr;
		if (!state.module) {
			state.status = std::format("runtime LoadLibrary failed ({})", ::GetLastError());
			state.runtimeRefused = true;
			return false;
		}
		if (!EqualPath(ModulePath(state.module), identity.path)) {
			state.status = "loaded NR module does not match the requested runtime path";
			state.runtimeRefused = true;
			return false;
		}
		if (identity.sha256 == kKernelRuntimeSha256 &&
			NeedsRuntimeKernels(ComputeCapability(a_info.device->GetAdapterLuid()))) {
			// Keep the requested Data path for siblings. MO2 can resolve the DLL
			// to Standard's physical folder while Universal supplies the pack.
			// identity.path identifies the module, not the merged asset directory.
			const auto packPath = a_info.runtimePath.parent_path() / std::filesystem::u8path(kKernelPackName);
			logger::info("[DLSSNR Source] compatibility kernel pack request={}", packPath.string());
			const auto result = ApplyRuntimeKernels(state.module, packPath, !resident);
			logger::info("[DLSSNR Source] compatibility kernels applied={} unsafe={} status={}", result.applied, result.unsafe, result.status);
			if (!result.applied) {
				state.status = result.status;
				state.runtimeRefused = !result.unsafe;
				return false;
			}
		}

		const auto dataPath = GetDataPath();
		const auto searchDirectory = identity.path.parent_path().wstring();
		const wchar_t* searchPath = searchDirectory.c_str();
		NVSDK_NGX_FeatureCommonInfo featureInfo{};
		featureInfo.PathListInfo.Path = &searchPath;
		featureInfo.PathListInfo.Length = 1;
		const auto publicInitResult = NVSDK_NGX_D3D12_Init_with_ProjectID(
			kTheosRenderPipelineNGXProjectId,
			NVSDK_NGX_ENGINE_TYPE_CUSTOM,
			kTheosRenderPipelineNGXEngineVersion,
			dataPath.c_str(),
			a_info.device,
			&featureInfo,
			NVSDK_NGX_Version_API);
		if (!IsUsableInitResult(publicInitResult)) {
			state.lastInitResult = static_cast<std::uint32_t>(publicInitResult);
			state.status = std::format(
				"public NGX initialization failed (0x{:08X})", state.lastInitResult);
			return false;
		}
		publicNGXInitialized.store(true, std::memory_order_release);
		// XeSS startup has no Streamline/DLSS bootstrap. The public NGX init
		// above must establish NR's loader independently before we resolve the
		// path used by the feature's scoped module-path hook.
		const auto normalLoader = FindNormalLoader();
		const auto normalLoaderPath = ModulePath(normalLoader);
		if (!normalLoader || normalLoaderPath.empty()) {
			state.status = "normal NGX loader is not resident after public initialization";
			return false;
		}
		logger::info("[DLSSNR Source] public NGX initialized result=0x{:08X} loader={}",
			static_cast<std::uint32_t>(publicInitResult), normalLoaderPath.string());

		auto init = reinterpret_cast<D3D12Init>(
			::GetProcAddress(state.module, "NVSDK_NGX_D3D12_Init"));
		auto initExt = reinterpret_cast<D3D12InitExt>(
			::GetProcAddress(state.module, "NVSDK_NGX_D3D12_Init_Ext"));
		auto getCapabilities = reinterpret_cast<D3D12GetCapabilityParameters>(
			::GetProcAddress(state.module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
		state.destroyParameters = reinterpret_cast<D3D12DestroyParameters>(
			::GetProcAddress(state.module, "NVSDK_NGX_D3D12_DestroyParameters"));
		state.createFeature = reinterpret_cast<D3D12CreateFeature>(
			::GetProcAddress(state.module, "NVSDK_NGX_D3D12_CreateFeature"));
		auto evaluateFeatureC = reinterpret_cast<D3D12EvaluateFeature>(
			::GetProcAddress(state.module, "NVSDK_NGX_D3D12_EvaluateFeature_C"));
		auto evaluateFeature = reinterpret_cast<D3D12EvaluateFeature>(
			::GetProcAddress(state.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
		state.evaluateFeature = evaluateFeatureC ? evaluateFeatureC : evaluateFeature;
		state.releaseFeature = reinterpret_cast<D3D12ReleaseFeature>(
			::GetProcAddress(state.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
		if ((!init && !initExt) || !state.createFeature || !state.evaluateFeature ||
			!state.releaseFeature) {
			state.status = "runtime feature export set is incomplete";
			return false;
		}

		state.modulePathHook = std::make_unique<ModulePathHook>();
		if (!state.modulePathHook->Install(state.module, normalLoaderPath)) {
			state.status = "module-path import hook could not be installed";
			return false;
		}
		auto initResult = NVSDK_NGX_Result_FAIL_NotInitialized;
		if (init) {
			initResult = init(
				kTheosRenderPipelineGenericCmsId, dataPath.c_str(), a_info.device, kFeatureApiVersion);
		}
		if (NVSDK_NGX_FAILED(initResult) && initExt) {
			initResult = initExt(
				kTheosRenderPipelineGenericCmsId,
				dataPath.c_str(),
				a_info.device,
				kFeatureApiVersion,
				nullptr);
		}
		state.lastInitResult = static_cast<std::uint32_t>(initResult);
		if (!IsUsableInitResult(initResult)) {
			state.status = std::format(
				"runtime initialization failed (0x{:08X})", state.lastInitResult);
			return false;
		}

		auto capabilityResult = NVSDK_NGX_Result_FAIL_NotInitialized;
		if (getCapabilities && state.destroyParameters) {
			capabilityResult = getCapabilities(&state.parameters);
		} else {
			capabilityResult = NVSDK_NGX_D3D12_GetCapabilityParameters(&state.parameters);
			state.destroyParameters = &NVSDK_NGX_D3D12_DestroyParameters;
		}
		if (NVSDK_NGX_FAILED(capabilityResult) || !state.parameters) {
			state.status = std::format(
				"runtime capability parameters were not created (0x{:08X})",
				static_cast<std::uint32_t>(capabilityResult));
			return false;
		}

		WriteCreationParameters(*state.parameters, state.contract);

		state.creationContext = std::make_unique<CreationContext>();
		auto& creationContext = *state.creationContext;
		if (!creationContext.Initialize(a_info.device)) {
			state.status = "feature creation context could not be created";
			return false;
		}
		const auto createResult = state.createFeature(
			creationContext.commandList.Get(),
			kNeuralRenderingFeature,
			state.parameters,
			&state.feature);
		state.lastCreateResult = static_cast<std::uint32_t>(createResult);
		if (NVSDK_NGX_FAILED(createResult) || !state.feature) {
			state.status = std::format(
				"feature creation failed (0x{:08X})", state.lastCreateResult);
			return false;
		}
		if (!creationContext.SubmitAndWait(
				a_info.handoffFence, a_info.handoffFenceValue)) {
			state.status = "feature creation GPU work did not complete";
			return false;
		}
		state.creationContext.reset(); // Its fence proved completion before release.

		state.device = a_info.device;
		state.runtimePath = identity.path;
		state.boundRuntimeRequest = a_info.runtimePath;
		state.displayWidth = a_info.displayWidth;
		state.displayHeight = a_info.displayHeight;
		state.renderWidth = a_info.renderWidth;
		state.renderHeight = a_info.renderHeight;
		state.initialized = true;
		state.status = "source-owned Neural Rendering feature initialized";
		logger::info("[DLSSNR Source] feature setup {:.0f} ms (runtime verified in {:.0f} ms)", elapsed(), verified);
		logger::warn(
			"[DLSSNR Source] feature initialized output={}x{} render={}x{} ratio={:.4f} preset={} build={} handle={}",
			state.displayWidth,
			state.displayHeight,
			state.renderWidth,
			state.renderHeight,
			state.contract.scalingRatio,
			state.contract.preset,
			RuntimeName(state.contract.build), static_cast<void*>(state.feature));
		return true;
	}

	bool FeatureSession::RecordEvaluation(const EvaluationInput& a_input)
	{
		auto& state = *state_;
		if (!state.initialized || !a_input.commandList || !a_input.color ||
			!a_input.motionVectors || !a_input.depth || !a_input.output ||
			!a_input.backbuffer) {
			state.status = "source evaluation inputs are incomplete";
			return false;
		}

		auto* parameters = state.parameters;
		parameters->Set("DLSSNR.Color", a_input.color);
		parameters->Set("DLSSNR.MVec", a_input.motionVectors);
		parameters->Set("DLSSNR.Depth", a_input.depth);
		parameters->Set("DLSSNR.Output", a_input.output);
		parameters->Set("DLSSNR.ControlMask", a_input.controlMask);
		parameters->Set("DLSSNR.Backbuffer", a_input.backbuffer);
		parameters->Set("DLSSNR.UI", a_input.ui);
		parameters->Set("DLSSNR.UIAlpha", a_input.uiAlpha);
		parameters->Set(
			"DLSSNR.BidirectionalDistortionField",
			a_input.bidirectionalDistortionField);
		SetSubrect(parameters, Subrect::Color, state.displayWidth, state.displayHeight);
		SetSubrect(parameters, Subrect::MVec, state.renderWidth, state.renderHeight);
		SetSubrect(parameters, Subrect::Depth, state.renderWidth, state.renderHeight);
		SetSubrect(parameters, Subrect::Output, state.displayWidth, state.displayHeight);
		SetSubrect(parameters, Subrect::Backbuffer, state.displayWidth, state.displayHeight);
		SetSubrect(
			parameters,
			Subrect::ControlMask,
			a_input.controlMask ? state.displayWidth : 0,
			a_input.controlMask ? state.displayHeight : 0);
		SetSubrect(
			parameters,
			Subrect::UI,
			a_input.ui ? static_cast<std::uint32_t>(a_input.ui->GetDesc().Width) : 0,
			a_input.ui ? a_input.ui->GetDesc().Height : 0);
		SetSubrect(
			parameters,
			Subrect::UIAlpha,
			a_input.uiAlpha ? state.displayWidth : 0,
			a_input.uiAlpha ? state.displayHeight : 0);
		SetSubrect(
			parameters,
			Subrect::BidirectionalDistortionField,
			a_input.bidirectionalDistortionField ? state.displayWidth : 0,
			a_input.bidirectionalDistortionField ? state.displayHeight : 0);
		parameters->Set("DLSSNR.MVecScaleX", a_input.motionVectorScaleX);
		parameters->Set("DLSSNR.MVecScaleY", a_input.motionVectorScaleY);
		WriteTuningParameters(*parameters, a_input.tuning, a_input.reset, a_input.depthInverted, state.contract.build);

		const auto evaluateResult = state.evaluateFeature(
			a_input.commandList, state.feature, parameters, nullptr);
		state.lastEvaluateResult = static_cast<std::uint32_t>(evaluateResult);
		if (NVSDK_NGX_FAILED(evaluateResult)) {
			state.status = std::format(
				"feature evaluation failed (0x{:08X})", state.lastEvaluateResult);
			return false;
		}
		++state.evaluationsRecorded;
		state.status = "source-owned Neural Rendering evaluation recorded";
		if (state.evaluationsRecorded == 1 || state.evaluationsRecorded % 600 == 0) {
			const auto tuning = UsesReconstructionContract(state.contract.build) ?
				SanitizeBuild14Tuning(a_input.tuning) : SanitizeTuning(a_input.tuning);
			logger::info(
				"[DLSSNR Source] evaluation recorded count={} reset={} depthInverted={} mvScale={:.1f}x{:.1f} style={} intensity={:.3f} localTone={:.3f} localStructure={:.3f} skinStructure={:.3f} autoMask={} uiCorrection={}",
				state.evaluationsRecorded,
				a_input.reset,
				a_input.depthInverted,
				a_input.motionVectorScaleX,
				a_input.motionVectorScaleY,
				tuning.style,
				tuning.intensity,
				tuning.localToneStrength,
				tuning.localStructureStrength,
				tuning.skinStructureStrength,
				tuning.useAutoSkinMask,
				tuning.uiCorrection);
		}
		return true;
	}

	bool FeatureSession::IsInitialized() const
	{
		return state_ && state_->initialized;
	}
	std::uint32_t FeatureSession::RefusedCreation() const
	{
		if (state_ && state_->runtimeRefused) { return static_cast<std::uint32_t>(NVSDK_NGX_Result_FAIL_FeatureNotSupported); }
		constexpr auto notCalled = (std::numeric_limits<std::uint32_t>::max)();
		return state_ && !state_->initialized && state_->lastCreateResult != notCalled &&
			NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(state_->lastCreateResult)) ? state_->lastCreateResult : 0;
	}
	RuntimeBuild FeatureSession::Build() const
	{
		return IsInitialized() ? state_->contract.build : RuntimeBuild::Unknown;
	}
	int FeatureSession::SubmittedPreset() const { return IsInitialized() ? state_->contract.preset : -1; }
	bool FeatureSession::ResetAfterRetirement()
	{
		if (state_->creationContext && !state_->creationContext->CanRelease()) { return false; }
		if (state_->feature && state_->releaseFeature) {
			const auto result = state_->releaseFeature(state_->feature);
			if (NVSDK_NGX_FAILED(result)) {
				state_->status = std::format("NR model replacement release refused (0x{:08X}); relaunch required", static_cast<std::uint32_t>(result));
				return false; // Retain feature, parameters and module; do not create another owner.
			}
			state_->feature = nullptr;
		}
		state_.reset(new State);
		return true;
	}

	std::uint64_t FeatureSession::EvaluationsRecorded() const
	{
		return state_ ? state_->evaluationsRecorded : 0;
	}

	const std::string& FeatureSession::Status() const
	{
		return state_->status;
	}

	bool PublicNGXInitializedInProcess()
	{
		return publicNGXInitialized.load(std::memory_order_acquire);
	}
}
