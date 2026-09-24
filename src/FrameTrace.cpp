#include "FrameTrace.h"

#include <PCH.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <vector>

namespace
{
	std::uint64_t Fnv1a64(std::string_view a_text)
	{
		std::uint64_t value = 14695981039346656037ull;
		for (const auto character : a_text) {
			value ^= static_cast<std::uint8_t>(character);
			value *= 1099511628211ull;
		}
		return value;
	}

	std::string TraceMetadata()
	{
		return std::format(
			R"({{"format":"TheosRenderPipeline frame trace","schema":1,"plugin":"{}","version":"{}","source_revision":"{}","d3d11_stage_schema":2,"clock":"QueryPerformanceCounter","present_semantics":"host Present submission, not physical scanout","gpu_stage_units":"nanoseconds","cpu_stage_units":"nanoseconds"}})",
			Plugin::NAME,
			Plugin::VERSION_STRING,
			Plugin::SOURCE_REVISION);
	}

	std::string TraceTimestamp()
	{
		const auto now = std::chrono::system_clock::now();
		const auto time = std::chrono::system_clock::to_time_t(now);
		std::tm local{};
		localtime_s(&local, &time);
		return std::format(
			"{:04}{:02}{:02}-{:02}{:02}{:02}",
			local.tm_year + 1900,
			local.tm_mon + 1,
			local.tm_mday,
			local.tm_hour,
			local.tm_min,
			local.tm_sec);
	}
}

FrameTrace::FrameTrace()
{
	for (std::size_t i = 0; i < queue_.size(); ++i) {
		queue_[i].sequence.store(i, std::memory_order_relaxed);
	}
}

FrameTrace::~FrameTrace()
{
	Stop();
}

std::int64_t FrameTrace::QueryQpc()
{
	LARGE_INTEGER now{};
	::QueryPerformanceCounter(&now);
	return now.QuadPart;
}

bool FrameTrace::SetEnabled(bool a_enabled)
{
	if (a_enabled) {
		if (Enabled()) {
			return !writerFailed_.load(std::memory_order_acquire);
		}
		return Start();
	}
	if (!Enabled() && !writerThread_.joinable()) {
		return true;
	}
	Stop();
	return true;
}

bool FrameTrace::Start()
{
	Stop();

	auto logDirectory = logger::log_directory();
	if (!logDirectory) {
		logger::error("[FrameTrace] standard SKSE log directory is unavailable");
		return false;
	}
	std::error_code error;
	const auto traceDirectory = *logDirectory / "TheosRenderPipeline Traces";
	std::filesystem::create_directories(traceDirectory, error);
	if (error) {
		logger::error("[FrameTrace] could not create trace directory: {}", error.message());
		return false;
	}
	outputPath_ = traceDirectory / std::format(
		"TheosRenderPipeline-{}-{}-{}.sfgtrace",
		TraceTimestamp(),
		::GetCurrentProcessId(),
		::GetTickCount64());
	output_.open(outputPath_, std::ios::binary | std::ios::trunc);
	if (!output_) {
		logger::error("[FrameTrace] could not open {}", outputPath_.string());
		outputPath_.clear();
		return false;
	}

	LARGE_INTEGER frequency{}, start{};
	::QueryPerformanceFrequency(&frequency);
	::QueryPerformanceCounter(&start);
	const auto unixNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const auto metadata = TraceMetadata();
	TraceHeaderV1 header{};
	std::memcpy(header.magic, "SFGTRC1", 7);
	header.version = 1;
	header.headerBytes = sizeof(header);
	header.recordBytes = sizeof(TraceEventV1);
	header.endian = 1;
	header.pointerBytes = sizeof(void*);
	header.qpcFrequency = static_cast<std::uint64_t>((std::max)(LONGLONG{ 0 }, frequency.QuadPart));
	header.startQpc = start.QuadPart;
	header.startUnixNs = unixNs;
	header.sessionId = static_cast<std::uint64_t>(start.QuadPart) ^ static_cast<std::uint64_t>(unixNs);
	header.pluginBuildId = Fnv1a64(Plugin::VERSION_STRING);
	header.metadataBytes = static_cast<std::uint32_t>(metadata.size());
	output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
	output_.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
	if (!output_) {
		logger::error("[FrameTrace] could not write trace header");
		output_.close();
		outputPath_.clear();
		return false;
	}

	bytesWritten_ = sizeof(header) + metadata.size();
	enqueuePosition_.store(0, std::memory_order_relaxed);
	dequeuePosition_ = 0;
	nextSequence_.store(0, std::memory_order_relaxed);
	recorded_.store(0, std::memory_order_relaxed);
	dropped_.store(0, std::memory_order_relaxed);
	written_.store(0, std::memory_order_relaxed);
	writerFailed_.store(false, std::memory_order_relaxed);
	stopRequested_.store(false, std::memory_order_relaxed);
	for (std::size_t i = 0; i < queue_.size(); ++i) {
		queue_[i].sequence.store(i, std::memory_order_relaxed);
	}
	enabled_.store(true, std::memory_order_release);
	writerThread_ = std::thread(&FrameTrace::WriterMain, this);
	logger::info("[FrameTrace] recording started: {}", outputPath_.string());
	return true;
}

void FrameTrace::Stop()
{
	if (!enabled_.load(std::memory_order_acquire) && !writerThread_.joinable()) {
		return;
	}
	if (enabled_.load(std::memory_order_acquire)) {
		Record(
			EventType::kSessionEnd,
			kSuccess,
			QueryQpc(),
			0,
			0,
			0,
			0,
			static_cast<std::int64_t>(recorded_.load(std::memory_order_relaxed)),
			static_cast<std::int64_t>(dropped_.load(std::memory_order_relaxed)));
	}
	enabled_.store(false, std::memory_order_release);
	stopRequested_.store(true, std::memory_order_release);
	writerCv_.notify_all();
	if (writerThread_.joinable()) {
		writerThread_.join();
	}
	if (output_.is_open()) {
		output_.flush();
		output_.close();
	}
	if (!outputPath_.empty()) {
		logger::info(
			"[FrameTrace] recording stopped: written={} dropped={} path={}",
			written_.load(std::memory_order_relaxed),
			dropped_.load(std::memory_order_relaxed),
			outputPath_.string());
	}
}

FrameTrace::Status FrameTrace::GetStatus() const
{
	return {
		enabled_.load(std::memory_order_acquire),
		writerRunning_.load(std::memory_order_acquire),
		writerFailed_.load(std::memory_order_acquire),
		recorded_.load(std::memory_order_relaxed),
		dropped_.load(std::memory_order_relaxed),
		written_.load(std::memory_order_relaxed),
		outputPath_.string()
	};
}

bool FrameTrace::Record(
	EventType a_type,
	std::uint16_t a_flags,
	std::int64_t a_qpc,
	std::uint64_t a_realFrameId,
	std::uint64_t a_correlationId,
	std::uint16_t a_phaseNumerator,
	std::uint16_t a_phaseDenominator,
	std::int64_t a_arg0,
	std::int64_t a_arg1)
{
	if (!Enabled()) {
		return false;
	}
	TraceEventV1 event{};
	event.type = static_cast<std::uint16_t>(a_type);
	event.flags = a_flags;
	event.size = sizeof(event);
	event.sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
	event.qpc = a_qpc > 0 ? a_qpc : QueryQpc();
	event.realFrameId = a_realFrameId;
	event.correlationId = a_correlationId;
	event.threadId = ::GetCurrentThreadId();
	event.phaseNumerator = a_phaseNumerator;
	event.phaseDenominator = a_phaseDenominator;
	event.arg0 = a_arg0;
	event.arg1 = a_arg1;
	if (!TryEnqueue(event)) {
		dropped_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const auto recorded = recorded_.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((recorded & 63ull) == 0) {
		writerCv_.notify_one();
	}
	return true;
}

bool FrameTrace::TryEnqueue(const TraceEventV1& a_event)
{
	auto position = enqueuePosition_.load(std::memory_order_relaxed);
	for (;;) {
		auto& cell = queue_[position % queue_.size()];
		const auto sequence = cell.sequence.load(std::memory_order_acquire);
		const auto difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
		if (difference == 0) {
			if (enqueuePosition_.compare_exchange_weak(
				position, position + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
				cell.event = a_event;
				cell.sequence.store(position + 1, std::memory_order_release);
				return true;
			}
		} else if (difference < 0) {
			return false;
		} else {
			position = enqueuePosition_.load(std::memory_order_relaxed);
		}
	}
}

bool FrameTrace::TryDequeue(TraceEventV1& a_event)
{
	auto& cell = queue_[dequeuePosition_ % queue_.size()];
	const auto sequence = cell.sequence.load(std::memory_order_acquire);
	const auto difference = static_cast<std::intptr_t>(sequence) -
		static_cast<std::intptr_t>(dequeuePosition_ + 1);
	if (difference != 0) {
		return false;
	}
	a_event = cell.event;
	cell.sequence.store(dequeuePosition_ + queue_.size(), std::memory_order_release);
	++dequeuePosition_;
	return true;
}

void FrameTrace::WriteDirect(const TraceEventV1& a_event)
{
	if (!output_ || bytesWritten_ + sizeof(a_event) > kMaximumTraceBytes) {
		writerFailed_.store(true, std::memory_order_release);
		enabled_.store(false, std::memory_order_release);
		stopRequested_.store(true, std::memory_order_release);
		return;
	}
	output_.write(reinterpret_cast<const char*>(&a_event), sizeof(a_event));
	if (!output_) {
		writerFailed_.store(true, std::memory_order_release);
		enabled_.store(false, std::memory_order_release);
		stopRequested_.store(true, std::memory_order_release);
		return;
	}
	bytesWritten_ += sizeof(a_event);
	written_.fetch_add(1, std::memory_order_relaxed);
}

void FrameTrace::WriterMain()
{
	writerRunning_.store(true, std::memory_order_release);
	::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	std::array<TraceEventV1, 1024> batch{};
	auto lastFlush = std::chrono::steady_clock::now();
	for (;;) {
		std::size_t count = 0;
		while (count < batch.size() && TryDequeue(batch[count])) {
			++count;
		}
		for (std::size_t i = 0; i < count; ++i) {
			WriteDirect(batch[i]);
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - lastFlush >= std::chrono::seconds(1)) {
			output_.flush();
			lastFlush = now;
		}
		if (stopRequested_.load(std::memory_order_acquire)) {
			TraceEventV1 remaining{};
			while (TryDequeue(remaining)) {
				WriteDirect(remaining);
			}
			break;
		}
		if (count == 0) {
			std::unique_lock lock(writerCvMutex_);
			writerCv_.wait_for(lock, std::chrono::milliseconds(100));
		}
	}
	output_.flush();
	writerRunning_.store(false, std::memory_order_release);
}
