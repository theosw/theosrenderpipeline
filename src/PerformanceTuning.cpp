#include "PerformanceTuning.h"

#include "FrameTrace.h"
#include "FrameTelemetry.h"
#include "VideoMemoryTelemetry.h"

#include <PCH.h>

#include <SimpleIni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace
{
	template <class T>
	constexpr auto ToIndex(T a_value)
	{
		return static_cast<std::size_t>(a_value);
	}

	constexpr const char* OptimizationName(PerformanceTuning::Optimization a_optimization)
	{
		switch (a_optimization) {
		case PerformanceTuning::Optimization::kDirectRCASOutput:
			return "direct RCAS output";
		case PerformanceTuning::Optimization::kDirectDLSSOutput:
			return "direct DLSS output";
		default:
			return "unknown optimization";
		}
	}

	float FpsFromMs(float a_ms)
	{
		return a_ms > 0.0f ? 1000.0f / a_ms : 0.0f;
	}

	std::int64_t QueryQpc()
	{
		LARGE_INTEGER now{};
		::QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	std::int64_t MillisecondsToNanoseconds(float a_ms)
	{
		return static_cast<std::int64_t>(std::llround(static_cast<double>(a_ms) * 1000000.0));
	}
}

void PerformanceTuning::LoadStartupINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto loadResult = ini.LoadFile(L"Data\\SKSE\\Plugins\\TheosRenderPipeline.ini");
	if (loadResult < 0) {
		logger::warn("[Performance] early INI load failed (rc={}); using default startup telemetry settings",
			static_cast<int>(loadResult));
		return;
	}
	Settings startup{};
	startup.enableGPUTimings = ini.GetBoolValue("Performance", "EnableGPUTimings", true);
	startup.enableFrameTrace = ini.GetBoolValue("Performance", "EnableFrameTrace", false);
	startup.directRCASOutput = ini.GetBoolValue("Performance", "DirectRCASOutput", false);
	startup.directDLSSOutput = ini.GetBoolValue("Performance", "DirectDLSSOutput", false);
	ApplySettings(startup);
}

void PerformanceTuning::ApplySettings(const Settings& a_settings)
{
	const Settings old = settings;
	Settings applied = a_settings;
	if (!FrameTrace::GetSingleton()->SetEnabled(a_settings.enableFrameTrace)) {
		applied.enableFrameTrace = false;
	}
	const bool oldTimingEnabled = old.enableGPUTimings || old.enableFrameTrace;
	const bool newTimingEnabled = applied.enableGPUTimings || applied.enableFrameTrace;
	if (newTimingEnabled != oldTimingEnabled) {
		ResetTimingWindow();
	}
	settings = applied;

	const std::array<bool, ToIndex(Optimization::kCount)> oldEnabled{
		old.directRCASOutput,
		old.directDLSSOutput
	};
	const std::array<bool, ToIndex(Optimization::kCount)> newEnabled{
		settings.directRCASOutput,
		settings.directDLSSOutput
	};
	for (std::size_t i = 0; i < newEnabled.size(); ++i) {
		auto& status = routeStatus_[i];
		status.requested = newEnabled[i];
		status.activeLastFrame = false;
		if (newEnabled[i] && !oldEnabled[i]) {
			status.sessionRejected = false;
			status.eligible = false;
			status.reason = "awaiting resource validation";
		} else if (!newEnabled[i]) {
			status.eligible = false;
			status.reason = "disabled";
		}
	}

	logger::info(
		"[Performance] settings: timings={} frameTrace={} directRCAS={} directDLSSOutput={}",
		settings.enableGPUTimings,
		settings.enableFrameTrace,
		settings.directRCASOutput,
		settings.directDLSSOutput);
}

void PerformanceTuning::ResetSessionFallbacks()
{
	for (auto& status : routeStatus_) {
		status.sessionRejected = false;
		status.eligible = false;
		status.activeLastFrame = false;
		status.reason = status.requested ? "awaiting resource validation" : "disabled";
	}
	logger::info("[Performance] session fallback latches reset");
}

void PerformanceTuning::BeginRouteFrame()
{
	const std::array<bool, ToIndex(Optimization::kCount)> requested{
		settings.directRCASOutput,
		settings.directDLSSOutput
	};
	for (std::size_t i = 0; i < requested.size(); ++i) {
		auto& status = routeStatus_[i];
		status.requested = requested[i];
		status.activeLastFrame = false;
		if (!requested[i] && status.reason != "disabled") {
			status.eligible = false;
			status.reason = "disabled";
		}
	}
}

bool PerformanceTuning::IsRouteAllowed(Optimization a_optimization) const
{
	const auto& status = routeStatus_[ToIndex(a_optimization)];
	return status.requested && !status.sessionRejected;
}

void PerformanceTuning::MarkRouteEligible(Optimization a_optimization, const char* a_reason)
{
	auto& status = routeStatus_[ToIndex(a_optimization)];
	if (status.sessionRejected) {
		return;
	}
	status.eligible = true;
	status.reason = a_reason ? a_reason : "resource contract accepted";
}

void PerformanceTuning::MarkRouteWaiting(Optimization a_optimization, const char* a_reason)
{
	auto& status = routeStatus_[ToIndex(a_optimization)];
	if (status.sessionRejected) {
		return;
	}
	status.eligible = false;
	status.activeLastFrame = false;
	status.reason = a_reason ? a_reason : "waiting for compatible mode";
}

void PerformanceTuning::MarkRouteActive(Optimization a_optimization)
{
	auto& status = routeStatus_[ToIndex(a_optimization)];
	status.eligible = true;
	status.activeLastFrame = true;
	++status.activeFrames;
	status.reason = "active";
}

void PerformanceTuning::MarkRouteFallback(Optimization a_optimization, const char* a_reason, bool a_rejectForSession)
{
	auto& status = routeStatus_[ToIndex(a_optimization)];
	status.eligible = false;
	status.activeLastFrame = false;
	status.sessionRejected = status.sessionRejected || a_rejectForSession;
	++status.fallbackCount;
	status.reason = a_reason ? a_reason : "original intermediate path selected";
	if (a_rejectForSession) {
		logger::warn(
			"[Performance] {} rejected for this session: {}",
			OptimizationName(a_optimization),
			status.reason);
	}
}

const PerformanceTuning::RouteStatus& PerformanceTuning::GetRouteStatus(Optimization a_optimization) const
{
	return routeStatus_[ToIndex(a_optimization)];
}

float PerformanceTuning::Smooth(float a_previous, float a_sample, bool a_initialized)
{
	return a_initialized ? a_previous * 0.9f + a_sample * 0.1f : a_sample;
}

void PerformanceTuning::PushPercentileSample(
	std::array<float, kPercentileWindow>& a_window,
	std::size_t& a_next,
	std::size_t& a_count,
	float a_ms)
{
	if (!std::isfinite(a_ms) || a_ms < 0.0f) {
		return;
	}
	a_window[a_next] = a_ms;
	a_next = (a_next + 1) % kPercentileWindow;
	a_count = a_count < kPercentileWindow ? a_count + 1 : kPercentileWindow;
}

PerformanceTuning::TimingSnapshot::Percentiles PerformanceTuning::SummarizePercentiles(
	const std::array<float, kPercentileWindow>& a_window,
	std::size_t a_count)
{
	TimingSnapshot::Percentiles result{};
	if (a_count > kPercentileWindow) {
		a_count = kPercentileWindow;
	}
	if (a_count == 0) {
		return result;
	}

	std::array<float, kPercentileWindow> sorted{};
	std::copy_n(a_window.begin(), a_count, sorted.begin());
	std::sort(sorted.begin(), sorted.begin() + a_count);
	auto percentile = [&](double a_quantile) {
		const double position = a_quantile * static_cast<double>(a_count - 1);
		const auto lower = static_cast<std::size_t>(position);
		const auto upper = lower + 1 < a_count ? lower + 1 : a_count - 1;
		const float fraction = static_cast<float>(position - static_cast<double>(lower));
		return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
	};

	result.p50Ms = percentile(0.50);
	result.p95Ms = percentile(0.95);
	result.p99Ms = percentile(0.99);
	result.samples = static_cast<std::uint32_t>(a_count);
	return result;
}

void PerformanceTuning::MaybeRefreshPercentiles()
{
	if (timingSnapshot_.d3d11Samples < lastPercentileRefreshSample_ + kPercentileRefreshSamples) {
		return;
	}
	lastPercentileRefreshSample_ = timingSnapshot_.d3d11Samples;
	timingSnapshot_.gameFrameCadence = SummarizePercentiles(gameFrameCadenceWindow_, gameFrameCadenceCount_);
	timingSnapshot_.d3d11Frame = SummarizePercentiles(d3d11FrameWindow_, d3d11FrameCount_);
}

void PerformanceTuning::ResetTimingWindow()
{
	++timingEpoch_;
	timingSnapshot_ = {};
	gameFrameCadenceWindow_.fill(0.0f);
	d3d11FrameWindow_.fill(0.0f);
	sourcePresentCpuWindow_.fill(0.0f);
	sourcePresentCpuNext_ = sourcePresentCpuCount_ = 0;
	gameFrameCadenceNext_ = 0;
	d3d11FrameNext_ = 0;
	gameFrameCadenceCount_ = 0;
	d3d11FrameCount_ = 0;
	lastPercentileRefreshSample_ = 0;
	lastLoggedTimingSample_ = 0;
	logger::info("[Performance] rolling timing window reset");
}

void PerformanceTuning::RecordSourcePresentCpuMs(float ms)
{
	if (!TimingEnabled() || !std::isfinite(ms) || ms < 0.0f) { return; }
	PushPercentileSample(sourcePresentCpuWindow_, sourcePresentCpuNext_, sourcePresentCpuCount_, ms);
	if (sourcePresentCpuCount_ == 1 || sourcePresentCpuNext_ % 30 == 0) {
		timingSnapshot_.sourcePresentCpu = SummarizePercentiles(sourcePresentCpuWindow_, sourcePresentCpuCount_);
	}
}

bool PerformanceTuning::EnsureD3D11Queries(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
{
	if (!a_device || !a_context) {
		return false;
	}
	if (timingD3D11Device_.Get() == a_device && timingD3D11Context_.Get() == a_context && d3d11Slots_[0].disjoint) {
		return !queryDiagnostics_.quarantined;
	}
	// The caller supplies a compatible device/context pair. Wrappers need not
	// return the same device pointer through GetDevice, so pointer identity is
	// only a cache key here, not admission. Check context type only on replacement.
	if (a_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
		++scopeDiagnostics_.contextMismatches;
		return false;
	}
	// Close through the retained owner before replacing a device/context pair.
	if (activeD3D11Slot_ >= 0) { EndD3D11Frame(timingD3D11Context_.Get()); }

	activeD3D11Slot_ = -1;
	nextD3D11Slot_ = 0;
	for (auto& slot : d3d11Slots_) {
		slot = {};
	}
	timingD3D11Device_ = a_device;
	timingD3D11Context_ = a_context;
	queryDiagnostics_.quarantined = false;
	queryDiagnostics_.failure = S_OK;
	// A new pair is a new measurement history, even without a settings reset.
	timingSnapshot_.d3d11Available.fill(false);
	timingSnapshot_.d3d11ScopeInvalid.fill(false);
	timingSnapshot_.d3d11Ms.fill(0.0f);
	auto fail = [&]() {
		for (auto& slot : d3d11Slots_) {
			slot = {};
		}
		timingD3D11Device_.Reset();
		timingD3D11Context_.Reset();
		return false;
	};

	D3D11_QUERY_DESC disjointDesc{};
	disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
	D3D11_QUERY_DESC timestampDesc{};
	timestampDesc.Query = D3D11_QUERY_TIMESTAMP;
	for (auto& slot : d3d11Slots_) {
		if (FAILED(a_device->CreateQuery(&disjointDesc, &slot.disjoint))) {
			logger::error("[Performance] failed to create D3D11 disjoint timing query");
			return fail();
		}
		for (auto& timestamp : slot.timestamps) {
			if (FAILED(a_device->CreateQuery(&timestampDesc, &timestamp))) {
				logger::error("[Performance] failed to create D3D11 timestamp query");
				return fail();
			}
		}
	}
	logger::info("[Performance] D3D11 GPU timing ring initialized");
	return true;
}

void PerformanceTuning::ResolveD3D11Queries(ID3D11DeviceContext* a_context)
{
	if (!a_context || queryDiagnostics_.quarantined) {
		return;
	}
	// Physical array order differs from recording order after ring wrap. Poll
	// at most four slots, oldest first; a pending oldest result ends this pass.
	for (std::size_t attempt = 0; attempt < kQuerySlots; ++attempt) {
		D3D11QuerySlot* oldest = nullptr;
		for (auto& candidate : d3d11Slots_) {
			if (candidate.pending && (!oldest || candidate.generation < oldest->generation)) { oldest = &candidate; }
		}
		if (!oldest) { break; }
		auto& slot = *oldest;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		const auto disjointResult = a_context->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (disjointResult == S_FALSE) { break; }
		if (disjointResult != S_OK) {
			QuarantineD3D11Queries(FAILED(disjointResult) ? disjointResult : E_UNEXPECTED);
			break;
		}
		if (disjoint.Disjoint || disjoint.Frequency == 0) {
			if (slot.timingEpoch == timingEpoch_) { PublishUnavailableD3D11Slot(slot); }
			slot.pending = false;
			continue;
		}

		std::array<std::uint64_t, kD3D11QueriesPerSlot> values{};
		bool ready = true;
		for (std::size_t stage = 0; stage < kD3D11StageCount && ready; ++stage) {
			if (!slot.recorded[stage] || slot.invalid[stage]) {
				continue;
			}
			for (std::size_t edge = 0; edge < 2; ++edge) {
				const auto query = stage * 2 + edge;
				const auto result = a_context->GetData(
					slot.timestamps[query].Get(),
					&values[query],
					sizeof(values[query]),
					D3D11_ASYNC_GETDATA_DONOTFLUSH);
				ready = result == S_OK;
				if (result != S_OK && result != S_FALSE) {
					QuarantineD3D11Queries(FAILED(result) ? result : E_UNEXPECTED);
				}
				if (!ready) {
					break;
				}
			}
		}
		if (!ready) {
			break;
		}
		// Preserve conservative readiness checks for recorded timestamps even
		// in old epochs. Disjoint readiness alone is not timestamp readiness.
		if (slot.timingEpoch != timingEpoch_) {
			slot.pending = false;
			continue;
		}

		++timingSnapshot_.d3d11Samples;
		const auto initialized = timingSnapshot_.d3d11Available;
		timingSnapshot_.lastCompletedFrameId = slot.frameId;
		timingSnapshot_.lastCompletedGeneration = slot.generation;
		timingSnapshot_.d3d11Available.fill(false);
		timingSnapshot_.d3d11ScopeInvalid = slot.invalid;
		for (std::size_t stage = 0; stage < kD3D11StageCount; ++stage) {
			if (!slot.recorded[stage] || slot.invalid[stage]) {
				timingSnapshot_.d3d11Ms[stage] = 0.0f;
				continue;
			}
			const auto begin = values[stage * 2];
			const auto end = values[stage * 2 + 1];
			if (end < begin) {
				timingSnapshot_.d3d11Ms[stage] = 0.0f;
				continue;
			}
			const float ms = static_cast<float>(static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency));
			timingSnapshot_.d3d11Ms[stage] = Smooth(timingSnapshot_.d3d11Ms[stage], ms, initialized[stage]);
			timingSnapshot_.d3d11Available[stage] = true;
			FrameTrace::GetSingleton()->Record(
				FrameTrace::EventType::kGpuStage,
				FrameTrace::kSuccess,
				slot.frameQpc,
				slot.frameId,
				static_cast<std::uint64_t>(FrameTrace::GpuDomain::kD3D11),
				static_cast<std::uint16_t>(stage),
				static_cast<std::uint16_t>(kD3D11StageCount),
				MillisecondsToNanoseconds(ms),
				static_cast<std::int64_t>(disjoint.Frequency));
			if (stage == ToIndex(D3D11Stage::kFrame)) {
				PushPercentileSample(d3d11FrameWindow_, d3d11FrameNext_, d3d11FrameCount_, ms);
			}
			if (stage == ToIndex(D3D11Stage::kNeuralEarlyRoundTrip) && slot.neuralCpuRecorded) {
				auto& totals = timingSnapshot_.neuralEarly;
				++totals.samples;
				totals.cpuNanoseconds += slot.neuralCpuNanoseconds;
				totals.gpuNanoseconds += static_cast<std::uint64_t>(MillisecondsToNanoseconds(ms));
				totals.allocatorWaits += slot.neuralWaited ? 1 : 0;
				totals.allocatorWaitNanoseconds += slot.neuralWaitNanoseconds;
				totals.cpuMaximumNanoseconds = (std::max)(totals.cpuMaximumNanoseconds, slot.neuralCpuNanoseconds);
				totals.allocatorWaitMaximumNanoseconds = (std::max)(totals.allocatorWaitMaximumNanoseconds, slot.neuralWaitNanoseconds);
			}
		}
		slot.pending = false;
	}
	MaybeRefreshPercentiles();
	MaybeLogTimingSummary();
}

void PerformanceTuning::PublishUnavailableD3D11Slot(const D3D11QuerySlot& slot)
{
	timingSnapshot_.lastCompletedFrameId = slot.frameId;
	timingSnapshot_.lastCompletedGeneration = slot.generation;
	timingSnapshot_.d3d11Available.fill(false);
	timingSnapshot_.d3d11Ms.fill(0.0f);
	timingSnapshot_.d3d11ScopeInvalid = slot.invalid;
}

void PerformanceTuning::QuarantineD3D11Queries(HRESULT failure)
{
	queryDiagnostics_.quarantined = true;
	queryDiagnostics_.failure = failure;
	++queryDiagnostics_.failures;
	timingSnapshot_.d3d11Available.fill(false);
	timingSnapshot_.d3d11ScopeInvalid.fill(false);
	timingSnapshot_.d3d11Ms.fill(0.0f);
	// Keep the uncertain queries reserved. No waits, reuse or automatic retry:
	// a later device/context replacement owns creation of a fresh ring.
	logger::warn("[Performance] GPU query failure 0x{:08X}; timings quarantined until device/context replacement (failures={})",
		static_cast<std::uint32_t>(failure), queryDiagnostics_.failures);
}

void PerformanceTuning::BeginD3D11Frame(
	ID3D11Device* a_device,
	ID3D11DeviceContext* a_context,
	std::uint64_t a_frameId,
	std::int64_t a_frameQpc)
{
	const bool hasPending = std::any_of(d3d11Slots_.begin(), d3d11Slots_.end(), [](const auto& a_slot) {
		return a_slot.pending;
	});
	if (!TimingEnabled() && !hasPending && activeD3D11Slot_ < 0) {
		return;
	}
	if (!a_device || !a_context) { return; }
	if (TimingEnabled()) {
		if (!EnsureD3D11Queries(a_device, a_context)) { return; }
	}
	if (activeD3D11Slot_ >= 0) { EndD3D11Frame(timingD3D11Context_.Get()); }
	ResolveD3D11Queries(timingD3D11Context_.Get());
	if (!TimingEnabled() || queryDiagnostics_.quarantined || recordingGeneration_ == (std::numeric_limits<std::uint64_t>::max)()) {
		return;
	}

	for (std::size_t attempt = 0; attempt < kQuerySlots; ++attempt) {
		const auto index = (nextD3D11Slot_ + attempt) % kQuerySlots;
		auto& slot = d3d11Slots_[index];
		if (slot.pending) {
			continue;
		}
		slot.started.fill(false);
		slot.recorded.fill(false);
		slot.invalid.fill(false);
		slot.generation = ++recordingGeneration_;
		slot.frameId = a_frameId;
		slot.neuralCpuRecorded = false;
		slot.timingEpoch = timingEpoch_;
		slot.frameQpc = a_frameQpc > 0 ? a_frameQpc : QueryQpc();
		a_context->Begin(slot.disjoint.Get());
		activeD3D11Slot_ = static_cast<int>(index);
		nextD3D11Slot_ = (index + 1) % kQuerySlots;
		slot.frameTicket = BeginD3D11Stage(a_context, D3D11Stage::kFrame);
		return;
	}
}

void PerformanceTuning::EndD3D11Frame(ID3D11DeviceContext* a_context)
{
	if (activeD3D11Slot_ < 0) {
		return;
	}
	if (a_context != timingD3D11Context_.Get()) {
		++scopeDiagnostics_.contextMismatches;
		return;
	}
	auto& slot = d3d11Slots_[static_cast<std::size_t>(activeD3D11Slot_)];
	for (std::size_t stage = 1; stage < kD3D11StageCount; ++stage) {
		if (slot.started[stage] && !slot.recorded[stage]) {
			slot.invalid[stage] = true;
			++scopeDiagnostics_.unclosedScopes;
		}
	}
	EndD3D11Stage(a_context, slot.frameTicket);
	a_context->End(slot.disjoint.Get());
	slot.pending = true;
	activeD3D11Slot_ = -1;
}

void PerformanceTuning::RecordNeuralEarlyCPU(std::uint64_t cpuNanoseconds, std::uint64_t waitNanoseconds, bool waited)
{
	if (!TimingEnabled() || activeD3D11Slot_ < 0) { return; }
	auto& slot = d3d11Slots_[static_cast<std::size_t>(activeD3D11Slot_)];
	if (!slot.started[ToIndex(D3D11Stage::kNeuralEarlyRoundTrip)] ||
		slot.invalid[ToIndex(D3D11Stage::kNeuralEarlyRoundTrip)] || slot.neuralCpuRecorded) { return; }
	slot.neuralCpuNanoseconds = cpuNanoseconds;
	slot.neuralWaitNanoseconds = waitNanoseconds;
	slot.neuralWaited = waited;
	slot.neuralCpuRecorded = true;
}

PerformanceTuning::D3D11StageTicket PerformanceTuning::BeginD3D11Stage(ID3D11DeviceContext* a_context, D3D11Stage a_stage)
{
	if (activeD3D11Slot_ < 0 || !a_context) { return {}; }
	auto& slot = d3d11Slots_[static_cast<std::size_t>(activeD3D11Slot_)];
	const auto stage = ToIndex(a_stage);
	if (stage >= kD3D11StageCount) {
		++scopeDiagnostics_.invalidStages;
		return {};
	}
	if (slot.started[stage]) {
		slot.invalid[stage] = true;
		++scopeDiagnostics_.conflictingStarts;
		return {};
	}
	a_context->End(slot.timestamps[stage * 2].Get());
	slot.started[stage] = true;
	return {slot.generation, a_stage, a_context};
}

bool PerformanceTuning::EndD3D11Stage(ID3D11DeviceContext* a_context, D3D11StageTicket a_ticket)
{
	if (!a_ticket) { return false; }
	if (activeD3D11Slot_ < 0) {
		++scopeDiagnostics_.rejectedEnds;
		return false;
	}
	auto& slot = d3d11Slots_[static_cast<std::size_t>(activeD3D11Slot_)];
	// A stale ticket must not invalidate or end any stage in the new frame.
	if (a_ticket.generation_ != slot.generation) {
		++scopeDiagnostics_.rejectedEnds;
		return false;
	}
	if (a_context != a_ticket.context_) {
		++scopeDiagnostics_.contextMismatches;
		return false;
	}
	const auto stage = ToIndex(a_ticket.stage_);
	if (slot.recorded[stage]) {
		slot.invalid[stage] = true;
		++scopeDiagnostics_.rejectedEnds;
		return false;
	}
	if (!slot.invalid[stage]) { a_context->End(slot.timestamps[stage * 2 + 1].Get()); }
	slot.recorded[stage] = true;
	return true;
}

void PerformanceTuning::RecordGameFrameCadenceMs(float a_ms)
{
	if (!TimingEnabled()) {
		return;
	}
	PushPercentileSample(gameFrameCadenceWindow_, gameFrameCadenceNext_, gameFrameCadenceCount_, a_ms);
}

void PerformanceTuning::MaybeLogTimingSummary()
{
	// Offline tracing already writes the frame-aligned binary stream. Avoid a
	// second synchronous text-log path unless live timing diagnostics are also
	// explicitly enabled.
	if (!settings.enableGPUTimings || timingSnapshot_.d3d11Samples < 1 ||
		timingSnapshot_.d3d11Samples < lastLoggedTimingSample_ + 120) {
		return;
	}
	lastLoggedTimingSample_ = timingSnapshot_.d3d11Samples;
	const auto& d11 = timingSnapshot_.d3d11Ms;
	const auto d11ms = [&](D3D11Stage a_stage) {
		if (timingSnapshot_.d3d11ScopeInvalid[ToIndex(a_stage)]) { return std::string{"invalid scope"}; }
		return TheosRenderPipeline::Telemetry::Milliseconds(d11[ToIndex(a_stage)], timingSnapshot_.d3d11Available[ToIndex(a_stage)]);
	};
	logger::info(
		"[Performance] smoothed ms: D3D11 frame={} inputs={} colorCopy={} mask={} DLSS={} RCAS={} upscalerOutputCopy={} presentationCopy={} hudless={}",
		d11ms(D3D11Stage::kFrame), d11ms(D3D11Stage::kFrameGenInputs),
		d11ms(D3D11Stage::kInputColorCopy), d11ms(D3D11Stage::kMaskEncode),
		d11ms(D3D11Stage::kDLSS), d11ms(D3D11Stage::kRCAS),
		d11ms(D3D11Stage::kOutputCopy), d11ms(D3D11Stage::kPresentationCopy), d11ms(D3D11Stage::kHUDLessCopy));
	const auto& scopes = scopeDiagnostics_;
	if (scopes.conflictingStarts || scopes.unclosedScopes || scopes.rejectedEnds || scopes.contextMismatches || scopes.invalidStages) {
		logger::info("[Performance] scope diagnostics (lifetime): conflictingStarts={} unclosed={} rejectedEnds={} contextMismatches={} invalidStages={}",
			scopes.conflictingStarts, scopes.unclosedScopes, scopes.rejectedEnds, scopes.contextMismatches, scopes.invalidStages);
	}
	{
		const auto& present = timingSnapshot_.sourcePresentCpu;
		logger::info("[Performance Source] nativeUI={} startupOverlay={} CPU Present p50={} p95={} p99={} samples={}; CPU includes API waits, not GPU generation or scanout",
			d11ms(D3D11Stage::kNativeUIComposition), d11ms(D3D11Stage::kStartupOverlayComposition),
			TheosRenderPipeline::Telemetry::Milliseconds(present.p50Ms, present.samples > 0),
			TheosRenderPipeline::Telemetry::Milliseconds(present.p95Ms, present.samples > 0),
			TheosRenderPipeline::Telemetry::Milliseconds(present.p99Ms, present.samples > 0), present.samples);
		logger::info("[Performance] NVIDIA generation GPU cost unavailable; source NR has separate fence-retired telemetry. D3D11 frame ends before Present; runtime output counts do not measure scanout cadence");
		const auto& nr = timingSnapshot_.neuralEarly;
		auto* memory = VideoMemoryTelemetry::GetSingleton();
		memory->Update(); // Sample with the menu closed too; internally rate-limited.
		const auto& vram = memory->GetSnapshot();
		logger::info("[Performance NR] epoch={} earlyGpuMs={} samples={} cpuTotalNs={} gpuTotalNs={} allocatorWaits={} allocatorWaitTotalNs={} cpuMaxNs={} allocatorWaitMaxNs={} vramAvailable={} vramUsageBytes={} vramBudgetBytes={}",
			timingEpoch_, d11ms(D3D11Stage::kNeuralEarlyRoundTrip), nr.samples,
			nr.cpuNanoseconds, nr.gpuNanoseconds, nr.allocatorWaits, nr.allocatorWaitNanoseconds,
			nr.cpuMaximumNanoseconds, nr.allocatorWaitMaximumNanoseconds,
			vram.available, vram.currentUsage, vram.budget);
	}

	const auto& cadence = timingSnapshot_.gameFrameCadence;
	const auto& gpuFrame = timingSnapshot_.d3d11Frame;
	logger::info(
		"[Performance] rolling percentiles ms: cadence p50={:.3f} p95={:.3f} p99={:.3f} (FPS equivalents {:.1f}/{:.1f}/{:.1f}, n={}); D3D11 frame p50={:.3f} p95={:.3f} p99={:.3f} (n={})",
		cadence.p50Ms,
		cadence.p95Ms,
		cadence.p99Ms,
		FpsFromMs(cadence.p50Ms),
		FpsFromMs(cadence.p95Ms),
		FpsFromMs(cadence.p99Ms),
		cadence.samples,
		gpuFrame.p50Ms,
		gpuFrame.p95Ms,
		gpuFrame.p99Ms,
		gpuFrame.samples);
}
