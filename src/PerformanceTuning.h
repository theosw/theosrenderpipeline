#pragma once

#include <d3d11_4.h>

#include <array>
#include <cstdint>
#include <string>
#include <wrl/client.h>

// Opt-in performance experiments and their measurement layer. All behavior-
// changing settings default off; the proven intermediate-copy path remains the
// fallback whenever a resource contract or an NGX evaluation is rejected.
class PerformanceTuning
{
public:
	struct Settings
	{
		bool enableGPUTimings{ true };
		bool enableFrameTrace{ false };
		bool directRCASOutput{ false };
		bool directDLSSOutput{ false };
	};

	enum class Optimization : std::uint8_t
	{
		kDirectRCASOutput,
		kDirectDLSSOutput,
		kCount
	};

	struct RouteStatus
	{
		bool requested{ false };
		bool eligible{ false };
		bool activeLastFrame{ false };
		bool sessionRejected{ false };
		std::uint64_t activeFrames{ 0 };
		std::uint64_t fallbackCount{ 0 };
		std::string reason{ "not evaluated" };
	};

	enum class D3D11Stage : std::uint8_t
	{
		kFrame,
		kFrameGenInputs,
		kInputColorCopy,
		kMaskEncode,
		kDLSS,
		kRCAS,
		kOutputCopy,
		kHUDLessCopy,
		// Append IDs to preserve existing trace stage numbering.
		kNativeUIComposition,
		kStartupOverlayComposition,
		kNeuralEarlyRoundTrip,
		kPresentationCopy,
		kCount
	};
	static_assert(static_cast<unsigned>(D3D11Stage::kOutputCopy) == 6);
	static_assert(static_cast<unsigned>(D3D11Stage::kPresentationCopy) == 11);

	// Only an accepted start can construct an owner. Generations are never reset
	// with timing windows, query slots or devices, so an old scope cannot end a
	// later recording even when the same slot and context are reused.
	class D3D11StageTicket
	{
	public:
		D3D11StageTicket() = default;
		explicit operator bool() const { return generation_ != 0; }
	private:
		friend class PerformanceTuning;
		D3D11StageTicket(std::uint64_t generation, D3D11Stage stage, ID3D11DeviceContext* context) :
			generation_(generation), stage_(stage), context_(context) {}
		std::uint64_t generation_{};
		D3D11Stage stage_{};
		ID3D11DeviceContext* context_{};
	};

	struct ScopeDiagnostics
	{
		// Lifetime counters, independent of timing-window resets. Reported only
		// with the existing periodic summary; no per-scope formatting or logging.
		std::uint64_t conflictingStarts{}, unclosedScopes{}, rejectedEnds{}, contextMismatches{}, invalidStages{};
	};

	struct QueryDiagnostics
	{
		std::uint64_t failures{}; // Lifetime count; one diagnostic per quarantine.
		HRESULT failure{S_OK};
		bool quarantined{};
	};

	struct TimingSnapshot
	{
		// Paired CPU/GPU samples from successfully completed early NR stages.
		// Totals are for this timing epoch; subtract endpoints for a fixed window.
		struct NeuralEarlyTotals
		{
			std::uint64_t samples{}, cpuNanoseconds{}, gpuNanoseconds{};
			std::uint64_t allocatorWaits{}, allocatorWaitNanoseconds{};
			std::uint64_t cpuMaximumNanoseconds{}, allocatorWaitMaximumNanoseconds{};
		};
		NeuralEarlyTotals neuralEarly{};
		struct Percentiles
		{
			float p50Ms{ 0.0f };
			float p95Ms{ 0.0f };
			float p99Ms{ 0.0f };
			std::uint32_t samples{ 0 };
		};

		std::array<float, static_cast<std::size_t>(D3D11Stage::kCount)> d3d11Ms{};
		// A missing query is not a measured zero. Validity follows the latest
		// retired query slot; disabled/absent stages cannot keep stale timings.
		std::array<bool, static_cast<std::size_t>(D3D11Stage::kCount)> d3d11Available{};
		std::array<bool, static_cast<std::size_t>(D3D11Stage::kCount)> d3d11ScopeInvalid{};
		std::uint64_t d3d11Samples{ 0 };
		// Identity of the latest completed slot, including invalid/disjoint slots.
		// Pending work leaves this identity and its availability unchanged.
		std::uint64_t lastCompletedFrameId{}, lastCompletedGeneration{};
		Percentiles gameFrameCadence{};
		Percentiles d3d11Frame{};
		Percentiles sourcePresentCpu{};
	};

	static PerformanceTuning* GetSingleton()
	{
		static PerformanceTuning singleton;
		return &singleton;
	}

	Settings settings{};

	void ApplySettings(const Settings& a_settings);
	// Read only process-safe performance switches before renderer hooks install.
	// Full game settings are still loaded later by RenderPipeline.
	void LoadStartupINI();
	void ResetSessionFallbacks();
	void BeginRouteFrame();
	bool IsRouteAllowed(Optimization a_optimization) const;
	void MarkRouteEligible(Optimization a_optimization, const char* a_reason = "resource contract accepted");
	void MarkRouteWaiting(Optimization a_optimization, const char* a_reason);
	void MarkRouteActive(Optimization a_optimization);
	void MarkRouteFallback(Optimization a_optimization, const char* a_reason, bool a_rejectForSession);
	const RouteStatus& GetRouteStatus(Optimization a_optimization) const;

	void BeginD3D11Frame(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		std::uint64_t a_frameId = 0,
		std::int64_t a_frameQpc = 0);
	void EndD3D11Frame(ID3D11DeviceContext* a_context);
	// Internal callers must supply compatible immediate contexts on the frame's
	// queue. A host/game wrapper may expose a different interface: stage admission
	// retains that existing precondition, with no per-stage COM identity queries.
	[[nodiscard]] D3D11StageTicket BeginD3D11Stage(ID3D11DeviceContext* a_context, D3D11Stage a_stage);
	// True means this owner closed its scope; a conflicted scope still has no
	// publishable measurement. A rejected end never closes a different owner.
	bool EndD3D11Stage(ID3D11DeviceContext* a_context, D3D11StageTicket a_ticket);
	void RecordNeuralEarlyCPU(std::uint64_t cpuNanoseconds, std::uint64_t waitNanoseconds, bool waited);

	void RecordGameFrameCadenceMs(float a_ms);
	void RecordSourcePresentCpuMs(float ms);
	void ResetTimingWindow();
	bool TimingEnabled() const { return settings.enableGPUTimings || settings.enableFrameTrace; }
	const TimingSnapshot& GetTimingSnapshot() const { return timingSnapshot_; }
	const ScopeDiagnostics& GetScopeDiagnostics() const { return scopeDiagnostics_; }
	const QueryDiagnostics& GetQueryDiagnostics() const { return queryDiagnostics_; }

private:
	PerformanceTuning() = default;

	static constexpr std::size_t kQuerySlots = 4;
	static constexpr std::size_t kD3D11StageCount = static_cast<std::size_t>(D3D11Stage::kCount);
	static constexpr std::size_t kD3D11QueriesPerSlot = kD3D11StageCount * 2;
	static constexpr std::size_t kPercentileWindow = 1024;
	static constexpr std::uint64_t kPercentileRefreshSamples = 30;

	struct D3D11QuerySlot
	{
		Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
		std::array<Microsoft::WRL::ComPtr<ID3D11Query>, kD3D11QueriesPerSlot> timestamps;
		std::array<bool, kD3D11StageCount> started{};
		std::array<bool, kD3D11StageCount> recorded{};
		std::array<bool, kD3D11StageCount> invalid{};
		std::uint64_t generation{};
		D3D11StageTicket frameTicket{};
		std::uint64_t frameId{ 0 };
		std::uint64_t timingEpoch{ 0 };
		std::int64_t frameQpc{ 0 };
		std::uint64_t neuralCpuNanoseconds{}, neuralWaitNanoseconds{};
		bool neuralCpuRecorded{}, neuralWaited{};
		bool pending{ false };
	};

	bool EnsureD3D11Queries(ID3D11Device* a_device, ID3D11DeviceContext* a_context);
	void ResolveD3D11Queries(ID3D11DeviceContext* a_context);
	void QuarantineD3D11Queries(HRESULT failure);
	void PublishUnavailableD3D11Slot(const D3D11QuerySlot& slot);
	void PushPercentileSample(
		std::array<float, kPercentileWindow>& a_window,
		std::size_t& a_next,
		std::size_t& a_count,
		float a_ms);
	static TimingSnapshot::Percentiles SummarizePercentiles(
		const std::array<float, kPercentileWindow>& a_window,
		std::size_t a_count);
	void MaybeRefreshPercentiles();
	void MaybeLogTimingSummary();
	static float Smooth(float a_previous, float a_sample, bool a_initialized);

	std::array<RouteStatus, static_cast<std::size_t>(Optimization::kCount)> routeStatus_{};

	Microsoft::WRL::ComPtr<ID3D11Device> timingD3D11Device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> timingD3D11Context_;
	std::array<D3D11QuerySlot, kQuerySlots> d3d11Slots_{};
	std::uint64_t recordingGeneration_{};
	ScopeDiagnostics scopeDiagnostics_{};
	// Clear timing window / settings toggles do not retry a failed pair. Only
	// replacement of the device/context pair creates fresh query objects.
	QueryDiagnostics queryDiagnostics_{};
	int activeD3D11Slot_{ -1 };
	std::size_t nextD3D11Slot_{ 0 };

	TimingSnapshot timingSnapshot_{};
	std::uint64_t timingEpoch_{};
	std::array<float, kPercentileWindow> gameFrameCadenceWindow_{};
	std::array<float, kPercentileWindow> d3d11FrameWindow_{};
	std::array<float, kPercentileWindow> sourcePresentCpuWindow_{};
	std::size_t sourcePresentCpuNext_{}, sourcePresentCpuCount_{};
	std::size_t gameFrameCadenceNext_{ 0 };
	std::size_t d3d11FrameNext_{ 0 };
	std::size_t gameFrameCadenceCount_{ 0 };
	std::size_t d3d11FrameCount_{ 0 };
	std::uint64_t lastPercentileRefreshSample_{ 0 };
	std::uint64_t lastLoggedTimingSample_{ 0 };
};

class ScopedD3D11PerformanceStage
{
public:
	ScopedD3D11PerformanceStage(ID3D11DeviceContext* a_context, PerformanceTuning::D3D11Stage a_stage) :
		context_(a_context), ticket_(PerformanceTuning::GetSingleton()->BeginD3D11Stage(a_context, a_stage)) {}

	~ScopedD3D11PerformanceStage()
	{
		if (ticket_) { PerformanceTuning::GetSingleton()->EndD3D11Stage(context_, ticket_); }
	}

	ScopedD3D11PerformanceStage(const ScopedD3D11PerformanceStage&) = delete;
	ScopedD3D11PerformanceStage& operator=(const ScopedD3D11PerformanceStage&) = delete;

private:
	ID3D11DeviceContext* context_{ nullptr };
	PerformanceTuning::D3D11StageTicket ticket_{};
};
