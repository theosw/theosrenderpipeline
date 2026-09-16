#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

// Opt-in frame-aligned trace stream for the offline TheosRenderPipeline trace viewer.
// Producers never allocate, format text, wait for disk, or block on a mutex.
// A bounded lock-free queue hands fixed-size records to one low-priority writer.
class FrameTrace
{
public:
	enum class EventType : std::uint16_t
	{
		kSessionInfo = 0x0000,
		kFrameReady = 0x0001,
		kProviderReady = 0x0002,
		kFrameQueued = 0x0003,
		kWorkerStart = 0x0004,
		kHostPresent = 0x0005,
		kRealEndpoint = 0x0006,
		kGpuStage = 0x0007,
		kCpuStage = 0x0008,
		// Emitted when a complete published policy becomes active at an x3
		// frame-set boundary. correlationId is the publication epoch; Arg0/Arg1
		// are the previous/current X3PacingPolicy values.
		kPolicyChange = 0x0020,
		kModeChange = 0x0021,
		kBoundary = 0x0022,
		kSafety = 0x0023,
		kTraceDrop = 0x0024,
		kBenchmarkBoundary = 0x0025,
		// One internal FidelityFX GPU pass measured by the optional pass
		// profiler. Arg0 holds the duration in nanoseconds; Arg1 packs the
		// timestamp frequency (bits 0-31), stable pass ID (32-43), queue type
		// (44-47), scope kind (48-51), and pass instance (52-63).
		kProviderPassStage = 0x0026,
		// Per-frame-set pass-profiler bookkeeping: Arg0 completed records, low
		// 32 bits of Arg1 samples dropped since the previous summary.
		kProviderPassSummary = 0x0027,
		// Absolute host/provider boundaries for the capture-to-provider-ready
		// critical path. phaseNumerator identifies ProviderTimelineStage; QPC is
		// always expressed in the trace header's clock domain.
		kProviderTimeline = 0x0028,
		kSessionEnd = 0x00FF
	};

	enum EventFlags : std::uint16_t
	{
		kSuccess = 0x0001,
		kGenerated = 0x0002,
		kReal = 0x0004,
		kPaced = 0x0008,
		kTimelineReanchored = 0x0010,
		kCatchUp = 0x0020,
		kInterpolationEligible = 0x0040,
		kSyntheticInput = 0x0080,
		kBoundaryBegin = 0x0100,
		kBoundaryEnd = 0x0200,
		kStableX2Fallback = 0x0400
	};

	enum class GpuDomain : std::uint16_t
	{
		kD3D11 = 11,
		kD3D12 = 12,
		// FidelityFX companion's private D3D12 queue; it may overlap host work.
		kProviderD3D12 = 13
	};

	enum class CpuStage : std::uint16_t
	{
		kGameCadence = 1,
		kRenderThreadPresent = 2,
		kFrameLimiter = 3,
		kEntryToEnqueue = 4,
		kQueueWait = 5,
		kWorkerService = 6,
		kProviderTransport = 7,
		kProviderAcquireWait = 8,
		kX3StagingRecord = 9
	};

	// correlationId namespace for kModeChange. Legacy traces used zero for x3
	// presentation; new traces identify the control that changed so offline A/B
	// segments do not depend on remembered wall-clock timestamps.
	enum class ModeChangeDomain : std::uint64_t
	{
		kX3Presentation = 1,
		kNeuralRendering = 2,
		kFrameGenerationRuntime = 3
	};

	enum class ProviderTimelineStage : std::uint16_t
	{
		kHostCommandSubmitted = 1,
		kHostInputFenceSignaled = 2,
		kProviderSubmitEntry = 3,
		kProviderQueueSubmitted = 4,
		kProviderGpuStart = 5,
		kProviderGpuEnd = 6,
		kProviderFenceObserved = 7,
		kHostProviderAcquireComplete = 8,
		kHostAsyncSlotComplete = 9,
		kHostCommandListReady = 10,
		kHostStagingRecorded = 11,
		kHostFrameGenPrepared = 12,
		kHostProviderCaptureRecorded = 13,
		kHostFrameGenLockAcquired = 14,
		kHostLegacyConfigComplete = 15,
		kHostFrameGenLockBypassed = 16,
		kHostLegacyConfigReused = 17
	};

	enum class BoundaryReason : std::uint16_t
	{
		kMainMenu = 1,
		kLoadingMenu = 2,
		kRaceMenu = 3,
		kFaderMenu = 4,
		kFocusOrSuspend = 5
	};

	struct TraceHeaderV1
	{
		char magic[8];
		std::uint16_t version;
		std::uint16_t headerBytes;
		std::uint16_t recordBytes;
		std::uint8_t endian;
		std::uint8_t pointerBytes;
		std::uint64_t qpcFrequency;
		std::int64_t startQpc;
		std::int64_t startUnixNs;
		std::uint64_t sessionId;
		std::uint64_t pluginBuildId;
		std::uint32_t displayWidth;
		std::uint32_t displayHeight;
		std::uint32_t renderWidth;
		std::uint32_t renderHeight;
		std::uint32_t refreshMilliHz;
		std::uint32_t metadataBytes;
		std::uint64_t reserved[2];
	};

	struct TraceEventV1
	{
		std::uint16_t type;
		std::uint16_t flags;
		std::uint32_t size;
		std::uint64_t sequence;
		std::int64_t qpc;
		std::uint64_t realFrameId;
		std::uint64_t correlationId;
		std::uint32_t threadId;
		std::uint16_t phaseNumerator;
		std::uint16_t phaseDenominator;
		std::int64_t arg0;
		std::int64_t arg1;
	};

	struct Status
	{
		bool enabled{ false };
		bool writerRunning{ false };
		bool writerFailed{ false };
		std::uint64_t recorded{ 0 };
		std::uint64_t dropped{ 0 };
		std::uint64_t written{ 0 };
		std::string path;
	};

	static FrameTrace* GetSingleton()
	{
		static FrameTrace trace;
		return &trace;
	}

	bool SetEnabled(bool a_enabled);
	bool Enabled() const { return enabled_.load(std::memory_order_acquire); }
	Status GetStatus() const;

	bool Record(
		EventType a_type,
		std::uint16_t a_flags = 0,
		std::int64_t a_qpc = 0,
		std::uint64_t a_realFrameId = 0,
		std::uint64_t a_correlationId = 0,
		std::uint16_t a_phaseNumerator = 0,
		std::uint16_t a_phaseDenominator = 0,
		std::int64_t a_arg0 = 0,
		std::int64_t a_arg1 = 0);

private:
	FrameTrace();
	~FrameTrace();
	FrameTrace(const FrameTrace&) = delete;
	FrameTrace& operator=(const FrameTrace&) = delete;

	static constexpr std::size_t kQueueCapacity = 8192;
	static constexpr std::uint64_t kMaximumTraceBytes = 256ull * 1024ull * 1024ull;

	struct QueueCell
	{
		std::atomic<std::size_t> sequence{ 0 };
		TraceEventV1 event{};
	};

	bool Start();
	void Stop();
	bool TryEnqueue(const TraceEventV1& a_event);
	bool TryDequeue(TraceEventV1& a_event);
	void WriterMain();
	void WriteDirect(const TraceEventV1& a_event);
	static std::int64_t QueryQpc();

	std::array<QueueCell, kQueueCapacity> queue_{};
	std::atomic<std::size_t> enqueuePosition_{ 0 };
	std::size_t dequeuePosition_{ 0 };
	std::atomic<std::uint64_t> nextSequence_{ 0 };
	std::atomic<std::uint64_t> recorded_{ 0 };
	std::atomic<std::uint64_t> dropped_{ 0 };
	std::atomic<std::uint64_t> written_{ 0 };
	std::atomic_bool enabled_{ false };
	std::atomic_bool stopRequested_{ false };
	std::atomic_bool writerRunning_{ false };
	std::atomic_bool writerFailed_{ false };
	std::thread writerThread_;
	std::condition_variable writerCv_;
	std::mutex writerCvMutex_;
	std::ofstream output_;
	std::filesystem::path outputPath_;
	std::uint64_t bytesWritten_{ 0 };
};

static_assert(sizeof(FrameTrace::TraceHeaderV1) == 96);
static_assert(sizeof(FrameTrace::TraceEventV1) == 64);
