#include "NeuralRenderingFeatureSession.h"

#include "NeuralRenderingCreationContext.h"
#include "NeuralRenderingBottleneck.h"
#include "NeuralRenderingModulePathHook.h"
#include "NeuralRenderingRuntimeIdentity.h"

#include <PCH.h>

#include <nvsdk_ngx.h>

#include <array>
#include <cmath>
#include <format>
#include <limits>
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

		void SetSubrect(
			NVSDK_NGX_Parameter* a_parameters,
			const char* a_prefix,
			std::uint32_t a_width,
			std::uint32_t a_height)
		{
			a_parameters->Set(std::format("DLSSNR.{}SubrectBaseX", a_prefix).c_str(), 0u);
			a_parameters->Set(std::format("DLSSNR.{}SubrectBaseY", a_prefix).c_str(), 0u);
			a_parameters->Set(
				std::format("DLSSNR.{}SubrectWidth", a_prefix).c_str(), a_width);
			a_parameters->Set(
				std::format("DLSSNR.{}SubrectHeight", a_prefix).c_str(), a_height);
		}

	}

	struct FeatureSession::State
	{
		std::unique_ptr<CreationContext> creationContext;
		std::unique_ptr<BottleneckReuse> bottleneck;
		bool bottleneckRequested{};
		std::uint64_t bottleneckReused{};
		std::string bottleneckStatus;
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
		std::string status{ "not initialized" };

		~State()
		{
			if (feature && releaseFeature) {
				releaseFeature(feature);
			}
			if (parameters && destroyParameters) {
				destroyParameters(parameters);
			}
			modulePathHook.reset();
			if (module && ownsModule) {
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
				state.bottleneckRequested == a_info.bottleneckReuse &&
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
		if (!a_info.device || a_info.runtimePath.empty() || a_info.displayWidth == 0 ||
			a_info.displayHeight == 0 || a_info.renderWidth == 0 || a_info.renderHeight == 0) {
			state.status = "source session inputs are incomplete";
			return false;
		}

		const auto identity = VerifySource(a_info.runtimePath, a_info.allowReconstructionRuntime);
		if (!identity.matched) {
			state.status = "source session runtime identity does not match the pinned build";
			return false;
		}
		state.contract = MakeFeatureContract(MatchRuntime(identity.size, identity.sha256, a_info.allowReconstructionRuntime),
			a_info.displayWidth, a_info.displayHeight, a_info.renderWidth, a_info.renderHeight, a_info.networkPreset);
		if (!state.contract.Valid()) {
			state.status = "source session feature contract is invalid";
			return false;
		}
		logger::info("[DLSSNR Source] pinned runtime build={} sha256={} path={}",
			RuntimeName(state.contract.build), identity.sha256, identity.path.string());
		const auto normalLoader = FindNormalLoader();
		const auto normalLoaderPath = ModulePath(normalLoader);
		if (!normalLoader || normalLoaderPath.empty()) {
			state.status = "normal NGX loader is not resident";
			return false;
		}
		if (const auto existing = ::GetModuleHandleW(L"nvngx_dlssnr.dll")) {
			if (!EqualPath(ModulePath(existing), identity.path)) {
				state.status = "a different nvngx_dlssnr.dll is already resident";
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

		// Each session needs its own reference, including a second NR pass that
		// may outlive the first session after an unfinished creation submission.
		state.module = ::LoadLibraryExW(
			identity.path.c_str(),
			nullptr,
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		state.ownsModule = state.module != nullptr;
		if (!state.module) {
			state.status = std::format("runtime LoadLibrary failed ({})", ::GetLastError());
			return false;
		}

		// Observe creation even when reuse is off, since this runtime caches kernel
		// handles across feature generations. No evaluation filtering without opt-in.
		state.bottleneckRequested = a_info.bottleneckReuse;
		const bool bridge = state.contract.build == RuntimeBuild::Nexus3108 && BottleneckReuse::Install(state.module);
		if (a_info.bottleneckReuse && bridge) state.bottleneck = std::make_unique<BottleneckReuse>();
		if (a_info.bottleneckReuse) logger::info("[NR Bottleneck] requested=true bridge={} build={}", bridge, RuntimeName(state.contract.build));

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
		SetSubrect(parameters, "Color", state.displayWidth, state.displayHeight);
		SetSubrect(parameters, "MVec", state.renderWidth, state.renderHeight);
		SetSubrect(parameters, "Depth", state.renderWidth, state.renderHeight);
		SetSubrect(parameters, "Output", state.displayWidth, state.displayHeight);
		SetSubrect(parameters, "Backbuffer", state.displayWidth, state.displayHeight);
		SetSubrect(
			parameters,
			"ControlMask",
			a_input.controlMask ? state.displayWidth : 0,
			a_input.controlMask ? state.displayHeight : 0);
		SetSubrect(
			parameters,
			"UI",
			a_input.ui ? static_cast<std::uint32_t>(a_input.ui->GetDesc().Width) : 0,
			a_input.ui ? a_input.ui->GetDesc().Height : 0);
		SetSubrect(
			parameters,
			"UIAlpha",
			a_input.uiAlpha ? state.displayWidth : 0,
			a_input.uiAlpha ? state.displayHeight : 0);
		SetSubrect(
			parameters,
			"BidirectionalDistortionField",
			a_input.bidirectionalDistortionField ? state.displayWidth : 0,
			a_input.bidirectionalDistortionField ? state.displayHeight : 0);
		parameters->Set("DLSSNR.MVecScaleX", a_input.motionVectorScaleX);
		parameters->Set("DLSSNR.MVecScaleY", a_input.motionVectorScaleY);
		WriteTuningParameters(*parameters, a_input.tuning, a_input.reset, a_input.depthInverted, state.contract.build);

		if (state.bottleneck && !state.bottleneck->Begin(a_input.commandList, a_input.reset)) {
			state.status = "NR bottleneck: " + state.bottleneck->Status(); return false;
		}
		const auto evaluateResult = state.evaluateFeature(
			a_input.commandList, state.feature, parameters, nullptr);
		state.lastEvaluateResult = static_cast<std::uint32_t>(evaluateResult);
		if (state.bottleneck) {
			const bool valid = state.bottleneck->End(NVSDK_NGX_SUCCEED(evaluateResult));
			if (state.bottleneck->Reused()) ++state.bottleneckReused;
			const auto& status = state.bottleneck->Status();
			if (!valid || (status != state.bottleneckStatus && status != "full evaluation" && status != "reused 42 coarse-stage kernels") ||
				(state.bottleneck->Reused() && state.bottleneckReused == 1) || state.evaluationsRecorded == 0 || state.evaluationsRecorded % 600 == 0) {
				logger::info("[NR Bottleneck] feature={} evaluations={} reused={} status={}",
					static_cast<void*>(state.feature), state.evaluationsRecorded + 1, state.bottleneckReused, status);
			}
			state.bottleneckStatus = status;
			if (!valid) { state.status = "NR bottleneck: " + status; return false; }
		}
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

	void FeatureSession::EvaluationSubmitted()
	{
		if (state_->bottleneck) state_->bottleneck->Submitted();
	}
	bool FeatureSession::BottleneckReused() const
	{
		return state_->bottleneck && state_->bottleneck->Reused();
	}
	bool FeatureSession::IsInitialized() const
	{
		return state_ && state_->initialized;
	}
	RuntimeBuild FeatureSession::Build() const
	{
		return IsInitialized() ? state_->contract.build : RuntimeBuild::Unknown;
	}

	std::uint64_t FeatureSession::EvaluationsRecorded() const
	{
		return state_ ? state_->evaluationsRecorded : 0;
	}

	const std::string& FeatureSession::Status() const
	{
		return state_->status;
	}
}
