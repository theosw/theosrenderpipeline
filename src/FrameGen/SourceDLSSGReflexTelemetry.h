#pragma once

#include <sl_reflex.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace TheosRenderPipeline::SourceDLSSG
{
	constexpr std::size_t ReflexTelemetryCapacity = 128;

	enum class ReflexTelemetryOutcome
	{
		NoReport,
		NoNewReport,
		NewReports,
		QueryFailure
	};

	struct ReflexTelemetryPoint
	{
		sl::ReflexReport report{};

		bool AppMarkersCompleteAndOrdered() const
		{
			const auto& r = report;
			return r.simStartTime && r.simEndTime && r.renderSubmitStartTime &&
				r.renderSubmitEndTime && r.presentStartTime && r.presentEndTime &&
				r.simStartTime <= r.simEndTime &&
				r.simEndTime <= r.renderSubmitStartTime &&
				r.renderSubmitStartTime <= r.renderSubmitEndTime &&
				r.renderSubmitEndTime <= r.presentStartTime &&
				r.presentStartTime <= r.presentEndTime;
		}

		bool DriverQueueCompleteAndOrdered() const
		{
			const auto& r = report;
			return r.driverStartTime && r.driverEndTime && r.osRenderQueueStartTime &&
				r.osRenderQueueEndTime && r.gpuRenderStartTime && r.gpuRenderEndTime &&
				r.driverStartTime <= r.driverEndTime &&
				r.osRenderQueueStartTime <= r.osRenderQueueEndTime &&
				r.gpuRenderStartTime <= r.gpuRenderEndTime;
		}
	};

	struct ReflexTelemetrySnapshot
	{
		std::array<ReflexTelemetryPoint, ReflexTelemetryCapacity> newestFirst{};
		std::size_t count{};
		std::uint64_t queries{};
		std::uint64_t queryFailures{};
		std::uint64_t acceptedReports{};
		std::uint64_t staleReports{};
		bool lowLatencyAvailable{};
		bool latencyReportAvailable{};
		bool flashIndicatorDriverControlled{};
		std::uint32_t statsWindowMessage{};
	};

	// The Reflex report contains marker, driver, OS queue and GPU timestamps.
	// It does not contain physical scanout time. Preserve the raw values and
	// frame identity so later consumers cannot silently relabel the boundary.
	class ReflexTelemetryTracker
	{
	public:
		ReflexTelemetryOutcome Sample(const sl::ReflexState& a_state)
		{
			++queries_;
			lowLatencyAvailable_ = a_state.lowLatencyAvailable;
			latencyReportAvailable_ = a_state.latencyReportAvailable;
			flashIndicatorDriverControlled_ = a_state.flashIndicatorDriverControlled;
			statsWindowMessage_ = a_state.statsWindowMessage;
			if (!a_state.latencyReportAvailable) { return ReflexTelemetryOutcome::NoReport; }

			std::array<const sl::ReflexReport*, sl::kReflexFrameReportCount> candidates{};
			std::size_t candidateCount{};
			for (const auto& report : a_state.frameReport) {
				if (report.presentStartTime && report.presentEndTime >= report.presentStartTime) {
					candidates[candidateCount++] = &report;
				}
			}
			std::sort(candidates.begin(), candidates.begin() + candidateCount,
				[](const auto* a, const auto* b) { return a->frameID < b->frameID; });

			std::size_t added{};
			for (std::size_t i = 0; i < candidateCount; ++i) {
				const auto& report = *candidates[i];
				if (hasLatest_ && report.frameID <= latestFrameID_) {
					++staleReports_;
					continue;
				}
				Push({ report });
				latestFrameID_ = report.frameID;
				hasLatest_ = true;
				++acceptedReports_;
				++added;
			}
			return added ? ReflexTelemetryOutcome::NewReports : ReflexTelemetryOutcome::NoNewReport;
		}

		void RecordQueryFailure()
		{
			++queries_;
			++queryFailures_;
		}

		ReflexTelemetrySnapshot Snapshot() const
		{
			ReflexTelemetrySnapshot snapshot{};
			snapshot.count = count_;
			snapshot.queries = queries_;
			snapshot.queryFailures = queryFailures_;
			snapshot.acceptedReports = acceptedReports_;
			snapshot.staleReports = staleReports_;
			snapshot.lowLatencyAvailable = lowLatencyAvailable_;
			snapshot.latencyReportAvailable = latencyReportAvailable_;
			snapshot.flashIndicatorDriverControlled = flashIndicatorDriverControlled_;
			snapshot.statsWindowMessage = statsWindowMessage_;
			for (std::size_t i = 0; i < count_; ++i) {
				snapshot.newestFirst[i] = Newest(i);
			}
			return snapshot;
		}

		const ReflexTelemetryPoint& Newest(std::size_t a_offset = 0) const
		{
			return points_[(head_ + ReflexTelemetryCapacity - a_offset) % ReflexTelemetryCapacity];
		}

	private:
		void Push(ReflexTelemetryPoint a_point)
		{
			head_ = count_ == 0 ? 0 : (head_ + 1) % ReflexTelemetryCapacity;
			if (count_ < ReflexTelemetryCapacity) { ++count_; }
			points_[head_] = a_point;
		}

		std::array<ReflexTelemetryPoint, ReflexTelemetryCapacity> points_{};
		std::size_t head_{};
		std::size_t count_{};
		std::uint64_t queries_{};
		std::uint64_t queryFailures_{};
		std::uint64_t acceptedReports_{};
		std::uint64_t staleReports_{};
		std::uint64_t latestFrameID_{};
		std::uint32_t statsWindowMessage_{};
		bool hasLatest_{};
		bool lowLatencyAvailable_{};
		bool latencyReportAvailable_{};
		bool flashIndicatorDriverControlled_{};
	};
}
