#pragma once

#include <cstdint>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include "SourceDLSSGGeneration.h"
#include "SourceDLSSGMFGContract.h"
#include "SourceDLSSGReflexTelemetry.h"
#include "SourceDLSSGOutputBatches.h"

namespace TheosRenderPipeline::SourceDLSSG
{
	constexpr bool GenerationEnabled(sl::DLSSGMode mode)
	{
		return mode == sl::DLSSGMode::eOn || mode == sl::DLSSGMode::eDynamic;
	}
	// The pinned runtime interprets this as an output cap with DLSS-G active.
	// Convert the requested number directly; do not scale by the FG multiplier.
	constexpr std::uint32_t OutputFrameLimitUs(int fps)
	{
		return fps > 0 && fps <= 1000 ? (1000000u + static_cast<unsigned>(fps) / 2) / static_cast<unsigned>(fps) : 0u;
	}
	constexpr bool ValidReflexMode(sl::ReflexMode mode)
	{
		return mode == sl::ReflexMode::eOff || mode == sl::ReflexMode::eLowLatency ||
			mode == sl::ReflexMode::eLowLatencyWithBoost;
	}
	constexpr const char* ReflexModeName(sl::ReflexMode mode)
	{
		switch (mode) {
		case sl::ReflexMode::eOff: return "Off";
		case sl::ReflexMode::eLowLatency: return "Low latency";
		case sl::ReflexMode::eLowLatencyWithBoost: return "Low latency + GPU boost";
		default: return "Invalid";
		}
	}
	// The owner supplies public Streamline entry points after initializing
	// Streamline and its device.
	struct SessionAPI
	{
		PFun_slGetNewFrameToken* newFrameToken{};
		PFun_slSetConstants* setConstants{};
		PFun_slSetTag* setTag{};
		PFun_slReflexSetOptions* setReflexOptions{};
		PFun_slReflexSleep* reflexSleep{};
		PFun_slReflexGetState* getReflexState{};
		PFun_slPCLSetMarker* marker{};
		PFun_slDLSSGSetOptions* setOptions{};
		PFun_slDLSSGGetState* getState{};

		// Must enqueue the D3D11 wait BEFORE the caller writes any frame inputs.
		// Called even when SL returns no fence, so the owner can bridge a barrier
		// on the presenting queue.
		// Returning false permanently stops this session.
		bool (*waitForInputReaders)(void* context, void* fence, std::uint64_t value){};
		void* context{};
		bool Complete() const;
	};

	struct TaggedTexture
	{
		sl::Resource resource{};
		sl::Extent extent{};
	};

	struct FrameGuides
	{
		TaggedTexture motion, depth, reactive, ui, hudless;
		std::uint32_t displayWidth{}, displayHeight{};
	};

	enum class SessionStage { Stopped, Simulation, Rendering, PresentPending, Faulted };
	enum class SessionFailure { None, InvalidAPI, InvalidSequence, InvalidInputs, Streamline, InputRetirement, Present };

	struct SessionSnapshot
	{
		SessionStage stage{ SessionStage::Stopped };
		SessionFailure failure{ SessionFailure::None };
		sl::Result result{ sl::Result::eOk };
		const char* operation{ "not started" };
		std::uint32_t frameIndex{};
		std::uint64_t stateQueries{};
		sl::Result stateQueryResult{ sl::Result::eOk };
		std::uint64_t stateWarnings{};
		// DLSSGState reports a delta since the previous getState call, not a
		// cumulative count or a configured multiplier. Sum populated post-Present
		// reads, including budget warnings; startup/resize state can repeat the
		// previous result and is excluded.
		std::uint64_t runtimePresentedFrames{};
		std::uint64_t presentationEpoch{ 1 };
		sl::DLSSGState state{};
		sl::DLSSGOptions options{};
		sl::ReflexMode reflexRequested{ sl::ReflexMode::eLowLatency };
		sl::ReflexMode reflexSubmitted{ sl::ReflexMode::eOff };
		std::uint32_t frameLimitRequestedUs{}, frameLimitSubmittedUs{};
		GenerationRequest generationRequested{};
		bool generationLimited{};
		bool mfgUnlockRequested{}, mfgUnlockReady{};

		// Submitted options alone do not establish an active session: also check
		// its lifecycle and feature status. A budget warning is diagnostic only.
		bool GenerationActive() const
		{
			return stage != SessionStage::Stopped && stage != SessionStage::Faulted &&
				state.status == sl::DLSSGStatus::eOk &&
				GenerationEnabled(options.mode);
		}
	};

	// One viewport, one Present thread. Runtime-owned generated Presents only.
	// Call Start once after device/feature initialization, Prepare during the
	// native world interval, CompleteInputWrites after the late UI copy, then
	// BeforePresent / native Present / AfterPresent. Never present if a method
	// fails. The owner stops rendering and retains resources on a failure.
	// DXGI_PRESENT_TEST bypasses this entire lifecycle through the test flag.
	class Session final
	{
	public:
		Session() = default;
		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;
		bool Start(const SessionAPI& a_api, std::uint32_t a_viewport);
		bool Prepare(const sl::Constants& a_constants, const FrameGuides& a_guides,
			sl::CommandBuffer* a_commandList);
		bool CompleteInputWrites();
		// Present-thread request only; no API call until the normal options slot.
		bool RequestReflexMode(sl::ReflexMode a_mode);
		void SetMFGUnlockState(bool requested, bool ready)
		{
			snapshot_.mfgUnlockRequested = requested;
			snapshot_.mfgUnlockReady = ready;
		}
		bool RequestGeneration(GenerationRequest a_request)
		{
			if (!ValidGenerationRequest(a_request)) { return false; }
			snapshot_.generationRequested = a_request;
			return true;
		}
		bool RequestOutputFPSLimit(int a_fps)
		{
			if (a_fps < 0 || a_fps > 1000) { return false; }
			snapshot_.frameLimitRequestedUs = OutputFrameLimitUs(a_fps);
			return true;
		}
		bool BeforePresent(bool a_enableGeneration, bool a_testOnly = false);
		bool AfterPresent(bool a_presentSucceeded, bool a_testOnly = false);
		// Stop generation before resize. The owner must then retire all GPU work
		// before destroying tagged textures. Resume the SAME session after rebuild
		// so frame tokens remain monotonic across resizes.
		bool Stop();
		bool ResumeAfterResize();
		const SessionSnapshot& Snapshot() const { return snapshot_; }
		ReflexTelemetrySnapshot ReflexTelemetry() const { return reflexTelemetry_.Snapshot(); }
		OutputBatchSnapshot OutputBatches() const { return outputBatches_.Snapshot(); }
		static bool ValidConstants(const sl::Constants& a_constants);
		static bool ValidGuides(const FrameGuides& a_guides);

	private:
		bool BeginFrame(bool a_afterPresent = false);
		bool ClearTags();
		bool SubmitOptions(const char* a_operation);
		bool Mark(sl::PCLMarker a_marker, const char* a_operation);
		bool Check(sl::Result a_result, const char* a_operation);
		bool Fail(SessionFailure a_failure, const char* a_operation);
		void QueryReflexState();
		SessionAPI api_{};
		sl::ViewportHandle viewport_{ 0u };
		sl::FrameToken* token_{};
		SessionSnapshot snapshot_{};
		FrameGuides guides_{};
		bool prepared_{ false };
		bool writesComplete_{ false };
		bool tagsLive_{ false };
		bool optionsPendingPresent_{ false };
		sl::DLSSGMode submittedMode_{ sl::DLSSGMode::eOff };
		std::uint32_t submittedGeneratedFrames_{ 1 };
		float submittedDynamicTarget_{};
		bool started_{ false };
		bool needsReset_{ true };
		ReflexTelemetryTracker reflexTelemetry_;
		OutputBatchTracker outputBatches_;
	};
}
