#include "SourceDLSSGSession.h"

#include <array>
#include <cmath>

namespace TheosRenderPipeline::SourceDLSSG
{
	static_assert(sl::kSDKVersion == 0x2000b0001fedcull, "Re-audit the pinned Streamline ABI before upgrading headers");
	static_assert(sizeof(sl::DLSSGOptions) == 0x78);
	static_assert(sizeof(sl::DLSSGState) == 0x58);

	namespace
	{
		constexpr std::array<sl::BufferType, 5> guideTypes{ sl::kBufferTypeMotionVectors, sl::kBufferTypeDepth,
			sl::kBufferTypeReactiveMaskHint, sl::kBufferTypeUIColorAndAlpha, sl::kBufferTypeHUDLessColor };
		bool Valid(float a_value) { return std::isfinite(a_value) && a_value != sl::INVALID_FLOAT; }
		bool Valid(sl::Boolean a_value) { return a_value == sl::eFalse || a_value == sl::eTrue; }
		bool Valid(const sl::float3& a_value) { return Valid(a_value.x) && Valid(a_value.y) && Valid(a_value.z); }
		bool Valid(const sl::float4x4& a_value)
		{
			for (const auto& row : a_value.row) {
				if (!Valid(row.x) || !Valid(row.y) || !Valid(row.z) || !Valid(row.w)) { return false; }
			}
			return true;
		}
		bool Valid(const TaggedTexture& a_texture, bool a_required)
		{
			if (!a_texture.resource.native) { return !a_required; }
			const auto& r = a_texture.resource;
			const auto& e = a_texture.extent;
			return r.type == sl::ResourceType::eTex2d && r.state != sl::INVALID_UINT &&
				e.width && e.height && e.left <= r.width && e.top <= r.height &&
				e.width <= r.width - e.left && e.height <= r.height - e.top;
		}
	}

	bool SessionAPI::Complete() const
	{
		return newFrameToken && setConstants && setTag && setReflexOptions &&
			reflexSleep && marker && setOptions && getState && waitForInputReaders;
	}

	bool Session::Fail(SessionFailure a_failure, const char* a_operation)
	{
		if (snapshot_.stage != SessionStage::Faulted) {
			snapshot_.failure = a_failure;
			snapshot_.operation = a_operation;
			snapshot_.stage = SessionStage::Faulted;
		}
		return false;
	}

	bool Session::Check(sl::Result a_result, const char* a_operation)
	{
		if (a_result == sl::Result::eOk) { return true; }
		snapshot_.result = a_result;
		return Fail(SessionFailure::Streamline, a_operation);
	}

	bool Session::Mark(sl::PCLMarker a_marker, const char* a_operation)
	{
		return token_ && Check(api_.marker(a_marker, *token_), a_operation);
	}

	bool Session::Start(const SessionAPI& a_api, std::uint32_t a_viewport)
	{
		if (started_ || snapshot_.stage != SessionStage::Stopped) { return Fail(SessionFailure::InvalidSequence, "Start"); }
		if (!a_api.Complete()) { return Fail(SessionFailure::InvalidAPI, "Start"); }
		started_ = true;
		api_ = a_api;
		viewport_ = sl::ViewportHandle(a_viewport);
		snapshot_.options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
		snapshot_.options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
		snapshot_.options.numFramesToGenerate = 1;
		// Caller owns the 600-Present host warm-up. This module never bypasses it.
		if (!SubmitOptions("initial DLSS-G options")) { return false; }
		sl::ReflexOptions reflex{};
		reflex.mode = snapshot_.reflexRequested;
		reflex.frameLimitUs = snapshot_.frameLimitRequestedUs;
		if (!Check(api_.setReflexOptions(reflex), "initial Reflex options")) { return false; }
		snapshot_.reflexSubmitted = reflex.mode;
		snapshot_.frameLimitSubmittedUs = reflex.frameLimitUs;
		QueryReflexState();
		return BeginFrame();
	}

	void Session::QueryReflexState()
	{
		if (!api_.getReflexState) { return; }
		sl::ReflexState state{};
		if (api_.getReflexState(state) == sl::Result::eOk) {
			reflexTelemetry_.Sample(state);
		} else {
			reflexTelemetry_.RecordQueryFailure();
		}
	}

	bool Session::SubmitOptions(const char* a_operation)
	{
		// Compare every mutable option. Skip only an identical submission with no
		// intervening real Present, not a multiplier change or a normal frame call.
		if (optionsPendingPresent_ && submittedMode_ == snapshot_.options.mode &&
			submittedGeneratedFrames_ == snapshot_.options.numFramesToGenerate &&
			submittedDynamicTarget_ == snapshot_.options.dynamicTargetFrameRate) { return true; }
		const auto result = api_.setOptions(viewport_, snapshot_.options);
		snapshot_.optionsResult = result;
		// NVIDIA stores the options before appending this advisory budget result.
		// Retrying would repeat an already-applied submission; keep normal Present
		// and input-reader retirement. Other results retain the fatal boundary.
		if (result == sl::Result::eWarnOutOfVRAM) { ++snapshot_.optionsWarnings; }
		else if (!Check(result, a_operation)) { return false; }
		submittedMode_ = snapshot_.options.mode;
		submittedGeneratedFrames_ = snapshot_.options.numFramesToGenerate;
		submittedDynamicTarget_ = snapshot_.options.dynamicTargetFrameRate;
		optionsPendingPresent_ = true;
		return true;
	}

	bool Session::BeginFrame(bool a_afterPresent)
	{
		// Query once at startup and once after each real Present, before D3D11
		// can reuse the shared input textures.
		sl::DLSSGState state{};
		const auto result = api_.getState(viewport_, state, nullptr);
		const bool warning = result == sl::Result::eWarnOutOfVRAM;
		if (!warning && !Check(result, "DLSS-G state")) { return false; }
		++snapshot_.stateQueries;
		snapshot_.stateQueryResult = result;
		// This budget warning is returned after NVIDIA populates the state,
		// including the input-reader fence. It does not change generation policy.
		if (warning) { ++snapshot_.stateWarnings; }
		snapshot_.state = state;
		if (a_afterPresent) {
			snapshot_.runtimePresentedFrames += state.numFramesActuallyPresented;
			OutputBatchMode mode = OutputBatchMode::Off;
			if (snapshot_.options.mode == sl::DLSSGMode::eOn) { mode = OutputBatchMode::Fixed; }
			else if (snapshot_.options.mode == sl::DLSSGMode::eDynamic) { mode = OutputBatchMode::Dynamic; }
			outputBatches_.Record(snapshot_.frameIndex - 1,
				{ mode, snapshot_.options.numFramesToGenerate,
					static_cast<std::uint32_t>(snapshot_.options.dynamicTargetFrameRate) },
				state.numFramesActuallyPresented);
		}
		if (a_afterPresent && snapshot_.frameIndex && snapshot_.frameIndex % 32 == 0) {
			QueryReflexState();
		}
		if (!api_.waitForInputReaders(api_.context, state.inputsProcessingCompletionFence,
			state.lastPresentInputsProcessingCompletionFenceValue)) {
			return Fail(SessionFailure::InputRetirement, "input reader fence");
		}
		token_ = nullptr;
		if (!Check(api_.newFrameToken(token_, &snapshot_.frameIndex), "frame token")) { return false; }
		if (!token_ || static_cast<std::uint32_t>(*token_) != snapshot_.frameIndex) {
			return Fail(SessionFailure::InvalidInputs, "frame token identity");
		}
		if (!Check(api_.reflexSleep(*token_), "Reflex sleep") ||
			!Mark(sl::PCLMarker::eSimulationStart, "SimulationStart")) { return false; }
		prepared_ = false;
		writesComplete_ = false;
		guides_ = {};
		snapshot_.stage = SessionStage::Simulation;
		snapshot_.operation = "simulation";
		return true;
	}

	bool Session::ValidConstants(const sl::Constants& c)
	{
		return Valid(c.cameraViewToClip) && Valid(c.clipToCameraView) &&
			Valid(c.clipToPrevClip) && Valid(c.prevClipToClip) &&
			Valid(c.jitterOffset.x) && Valid(c.jitterOffset.y) &&
			Valid(c.mvecScale.x) && Valid(c.mvecScale.y) && c.mvecScale.x != 0 && c.mvecScale.y != 0 &&
			Valid(c.cameraPos) && Valid(c.cameraUp) && Valid(c.cameraRight) && Valid(c.cameraFwd) &&
			Valid(c.cameraNear) && Valid(c.cameraFar) && c.cameraNear > 0 && c.cameraFar > c.cameraNear &&
			Valid(c.cameraFOV) && c.cameraFOV > 0 && c.cameraFOV < 3.141593f &&
			Valid(c.cameraAspectRatio) && c.cameraAspectRatio > 0 &&
			Valid(c.depthInverted) && Valid(c.cameraMotionIncluded) && Valid(c.motionVectors3D) &&
			Valid(c.reset) && Valid(c.orthographicProjection) && Valid(c.motionVectorsDilated) &&
			Valid(c.motionVectorsJittered) &&
			(c.cameraMotionIncluded == sl::eTrue || Valid(c.motionVectorsInvalidValue));
	}

	bool Session::ValidGuides(const FrameGuides& g)
	{
		if (!g.displayWidth || !g.displayHeight || !Valid(g.motion, true) || !Valid(g.depth, true) ||
			!Valid(g.reactive, false) || !Valid(g.ui, false) || !Valid(g.hudless, false)) { return false; }
		for (const auto* texture : { &g.ui, &g.hudless }) {
			if (texture->resource.native && (texture->extent.width != g.displayWidth ||
				texture->extent.height != g.displayHeight)) { return false; }
		}
		return true;
	}

	bool Session::Prepare(const sl::Constants& a_constants, const FrameGuides& a_guides,
		sl::CommandBuffer* a_commandList)
	{
		if (snapshot_.stage != SessionStage::Simulation) { return Fail(SessionFailure::InvalidSequence, "Prepare"); }
		if (!ValidConstants(a_constants) || !ValidGuides(a_guides) || !a_commandList) {
			return Fail(SessionFailure::InvalidInputs, "frame constants/guides");
		}
		auto constants = a_constants;
		if (needsReset_) { constants.reset = sl::eTrue; }
		if (!Mark(sl::PCLMarker::eSimulationEnd, "SimulationEnd") ||
			!Mark(sl::PCLMarker::eRenderSubmitStart, "RenderSubmitStart") ||
			!Check(api_.setConstants(constants, *token_, viewport_), "common constants")) { return false; }
		guides_ = a_guides;
		std::array<sl::ResourceTag, 5> tags;
		std::array<TaggedTexture*, 5> textures{ &guides_.motion, &guides_.depth, &guides_.reactive, &guides_.ui, &guides_.hudless };
		for (std::size_t i = 0; i < tags.size(); ++i) {
			// Null optional tags explicitly clear the previous frame's identity.
			tags[i] = sl::ResourceTag(textures[i]->resource.native ? &textures[i]->resource : nullptr,
				guideTypes[i], sl::ResourceLifecycle::eValidUntilPresent, &textures[i]->extent);
		}
		if (!Check(api_.setTag(viewport_, tags.data(), static_cast<std::uint32_t>(tags.size()), a_commandList), "frame tags")) { return false; }
		tagsLive_ = true;
		prepared_ = true;
		needsReset_ = false;
		snapshot_.stage = SessionStage::Rendering;
		snapshot_.operation = "awaiting late input writes";
		return true;
	}

	bool Session::ClearTags()
	{
		if (!tagsLive_) { return true; }
		std::array<sl::ResourceTag, guideTypes.size()> tags;
		for (std::size_t i = 0; i < tags.size(); ++i) {
			tags[i] = sl::ResourceTag(nullptr, guideTypes[i], sl::ResourceLifecycle::eValidUntilPresent);
		}
		// Legacy tags retain resource identities until replaced, nulled, or expired.
		// All-null tags need no command list. This only removes SL's references;
		// the owner must still retain its textures until GPU retirement is proven.
		if (!Check(api_.setTag(viewport_, tags.data(), static_cast<std::uint32_t>(tags.size()), nullptr), "clear frame tags")) { return false; }
		tagsLive_ = false;
		return true;
	}

	bool Session::CompleteInputWrites()
	{
		if (snapshot_.stage != SessionStage::Rendering) { return Fail(SessionFailure::InvalidSequence, "CompleteInputWrites"); }
		writesComplete_ = true;
		return true;
	}

	bool Session::RequestReflexMode(sl::ReflexMode a_mode)
	{
		if (!ValidReflexMode(a_mode)) { return false; }
		snapshot_.reflexRequested = a_mode;
		return true;
	}

	bool Session::BeforePresent(bool a_enableGeneration, bool a_testOnly)
	{
		if (a_testOnly) { return true; }
		if (snapshot_.stage != SessionStage::Simulation && snapshot_.stage != SessionStage::Rendering) {
			return Fail(SessionFailure::InvalidSequence, "BeforePresent");
		}
		if (snapshot_.stage == SessionStage::Simulation &&
			(!Mark(sl::PCLMarker::eSimulationEnd, "SimulationEnd") ||
			 !Mark(sl::PCLMarker::eRenderSubmitStart, "RenderSubmitStart"))) { return false; }
		if (!Mark(sl::PCLMarker::eRenderSubmitEnd, "RenderSubmitEnd")) { return false; }
		if ((!prepared_ || !writesComplete_) && !ClearTags()) { return false; }
		const auto& state = snapshot_.state;
		const bool enable = a_enableGeneration && prepared_ && writesComplete_ &&
			state.numFramesToGenerateMax >= 1 && state.status == sl::DLSSGStatus::eOk &&
			guides_.displayWidth >= state.minWidthOrHeight && guides_.displayHeight >= state.minWidthOrHeight;
		sl::ReflexOptions reflex{};
		// Keep low-latency mode on while generating. An Off preference resumes
		// when generation is actually off. Sleep and PCL markers always remain.
		reflex.mode = enable && snapshot_.reflexRequested == sl::ReflexMode::eOff ?
			sl::ReflexMode::eLowLatency : snapshot_.reflexRequested;
		reflex.frameLimitUs = snapshot_.frameLimitRequestedUs;
		if (!Check(api_.setReflexOptions(reflex), "Reflex options")) { return false; }
		snapshot_.reflexSubmitted = reflex.mode;
		snapshot_.frameLimitSubmittedUs = reflex.frameLimitUs;
		const auto selection = SelectGeneration(snapshot_.generationRequested,
			MFGContract::Maximum(snapshot_.mfgUnlockRequested, snapshot_.mfgUnlockReady, state.numFramesToGenerateMax),
			MFGContract::Dynamic(snapshot_.mfgUnlockRequested, snapshot_.mfgUnlockReady,
				state.bIsDynamicMFGSupported == sl::eTrue));
		snapshot_.generationLimited = selection.limited;
		snapshot_.options.numFramesToGenerate = selection.effective.generatedFrames;
		snapshot_.options.dynamicTargetFrameRate = static_cast<float>(selection.effective.dynamicTargetFPS);
		snapshot_.options.mode = !enable ? sl::DLSSGMode::eOff :
			selection.effective.dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
		if (!SubmitOptions("DLSS-G options") ||
			!Mark(sl::PCLMarker::ePresentStart, "PresentStart")) { return false; }
		snapshot_.stage = SessionStage::PresentPending;
		snapshot_.operation = enable ? "present with runtime frame generation" : "present without generation";
		return true;
	}

	bool Session::AfterPresent(bool a_presentSucceeded, bool a_testOnly)
	{
		if (a_testOnly) { return true; }
		if (snapshot_.stage != SessionStage::PresentPending) { return Fail(SessionFailure::InvalidSequence, "AfterPresent"); }
		if (!a_presentSucceeded) { return Fail(SessionFailure::Present, "native Present"); }
		optionsPendingPresent_ = false;
		if (!Mark(sl::PCLMarker::ePresentEnd, "PresentEnd")) { return false; }
		if (!prepared_ || !writesComplete_) { needsReset_ = true; }
		++snapshot_.frameIndex;
		return BeginFrame(true);
	}

	bool Session::Stop()
	{
		if (snapshot_.stage != SessionStage::Simulation && snapshot_.stage != SessionStage::Rendering) {
			return Fail(SessionFailure::InvalidSequence, "Stop");
		}
		snapshot_.options.mode = sl::DLSSGMode::eOff;
		if (!SubmitOptions("disable before teardown")) { return false; }
		if (!ClearTags()) { return false; }
		const bool simulation = snapshot_.stage == SessionStage::Simulation;
		if (!Mark(simulation ? sl::PCLMarker::eSimulationEnd : sl::PCLMarker::eRenderSubmitEnd,
			simulation ? "SimulationEnd" : "RenderSubmitEnd")) { return false; }
		snapshot_.stage = SessionStage::Stopped;
		++snapshot_.presentationEpoch; // Consumer must not average across teardown/resize.
		outputBatches_.BeginEpoch();
		snapshot_.operation = "retire resources before teardown";
		return true;
	}

	bool Session::ResumeAfterResize()
	{
		if (!started_ || snapshot_.stage != SessionStage::Stopped) { return Fail(SessionFailure::InvalidSequence, "ResumeAfterResize"); }
		++snapshot_.frameIndex;
		needsReset_ = true;
		return BeginFrame();
	}
}
