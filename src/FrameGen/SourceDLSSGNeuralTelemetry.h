#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace TheosRenderPipeline::SourceDLSSG
{
	inline constexpr std::size_t NeuralTelemetryCapacity = 128;

	struct NeuralTelemetryPoint
	{
		std::uint64_t evaluation{};
		std::uint64_t gpuTicks{};
		std::uint64_t gpuFrequency{};
		std::uint64_t cpuRecordNanoseconds{};
	};

	struct NeuralTelemetrySnapshot
	{
		std::array<NeuralTelemetryPoint, NeuralTelemetryCapacity> newestFirst{};
		std::size_t count{};
		std::uint64_t gpuSamples{};
		std::uint64_t gpuQueryFailures{};
		std::uint64_t gpuTicksTotal{};
		std::uint64_t gpuTicksMinimum{};
		std::uint64_t gpuTicksMaximum{};
		std::uint64_t gpuFrequency{};
		std::uint64_t cpuSamples{};
		std::uint64_t cpuRecordNanosecondsTotal{};
		std::uint64_t cpuRecordNanosecondsMaximum{};
		// Model rate: recorded frames that reused the previous change instead of
		// running the model, and the recent share of frames that ran it (0 to 1).
		std::uint64_t skippedFrames{};
		double modelRunShare{ 1 };
		// Latest completed recorded frames, including model skips. This is the
		// whole NR command span, not inference time divided by the model interval.
		std::size_t frameSamples{};
		std::uint64_t frameQueryFailures{}, frameTicksTotal{}, frameFrequency{};
		double AverageFrameGPUMicroseconds() const
		{
			return frameSamples && frameFrequency ? 1'000'000.0 * static_cast<double>(frameTicksTotal) /
				(static_cast<double>(frameSamples) * static_cast<double>(frameFrequency)) : 0.0;
		}

		double AverageGPUMicroseconds() const
		{
			return gpuSamples && gpuFrequency ?
				1'000'000.0 * static_cast<double>(gpuTicksTotal) /
				(static_cast<double>(gpuSamples) * static_cast<double>(gpuFrequency)) : 0.0;
		}
		double MaximumGPUMicroseconds() const
		{
			return gpuFrequency ? 1'000'000.0 * static_cast<double>(gpuTicksMaximum) /
				static_cast<double>(gpuFrequency) : 0.0;
		}
		double AverageCPURecordMicroseconds() const
		{
			return cpuSamples ? static_cast<double>(cpuRecordNanosecondsTotal) /
				(static_cast<double>(cpuSamples) * 1000.0) : 0.0;
		}
		double MaximumCPURecordMicroseconds() const
		{
			return static_cast<double>(cpuRecordNanosecondsMaximum) / 1000.0;
		}
	};

	// Measures the stable host/runtime boundary. GPU ticks cover commands emitted
	// by the EvaluateFeature sequence, including inter-pass barriers/background
	// seeding when two passes are selected. CPU time covers the same command
	// recording only. Neither value is a physical presentation or scanout time.
	class NeuralTelemetryTracker
	{
	public:
		void RecordFrame(std::uint64_t start, std::uint64_t end, std::uint64_t frequency)
		{
			if (!frequency || end < start || (frameFrequency_ && frequency != frameFrequency_)) {
				++frameQueryFailures_; return;
			}
			frameFrequency_ = frequency;
			frameTicksTotal_ -= frameTicks_[frameHead_];
			frameTicks_[frameHead_] = end - start;
			frameTicksTotal_ += end - start;
			frameHead_ = (frameHead_ + 1) % NeuralTelemetryCapacity;
			frameCount_ = (std::min)(frameCount_ + 1, NeuralTelemetryCapacity);
		}
		void Record(std::uint64_t a_evaluation, std::uint64_t a_startTick,
			std::uint64_t a_endTick, std::uint64_t a_frequency,
			std::uint64_t a_cpuRecordNanoseconds)
		{
			RecordCPU(a_cpuRecordNanoseconds);
			if (!a_frequency || a_endTick < a_startTick) {
				++gpuQueryFailures_;
				return;
			}
			const auto ticks = a_endTick - a_startTick;
			if (gpuFrequency_ && gpuFrequency_ != a_frequency) {
				// Mixing queue timestamp domains would make the aggregate meaningless.
				++gpuQueryFailures_;
				return;
			}
			gpuFrequency_ = a_frequency;
			gpuTicksTotal_ += ticks;
			gpuTicksMinimum_ = gpuSamples_ ? (std::min)(gpuTicksMinimum_, ticks) : ticks;
			gpuTicksMaximum_ = (std::max)(gpuTicksMaximum_, ticks);
			++gpuSamples_;
			Push({ a_evaluation, ticks, a_frequency, a_cpuRecordNanoseconds });
		}

		void RecordCPUOnly(std::uint64_t a_cpuRecordNanoseconds)
		{
			RecordCPU(a_cpuRecordNanoseconds);
			++gpuQueryFailures_;
		}

		// Once per recorded frame. GPU and CPU samples above cover model runs only.
		void RecordModelRun(bool a_ran)
		{
			if (!a_ran) { ++skippedFrames_; }
			// A moving average over roughly the last 16 frames.
			modelRunShare_ += ((a_ran ? 1.0 : 0.0) - modelRunShare_) / 16.0;
		}

		NeuralTelemetrySnapshot Snapshot() const
		{
			NeuralTelemetrySnapshot result{};
			result.count = count_;
			result.gpuSamples = gpuSamples_;
			result.gpuQueryFailures = gpuQueryFailures_;
			result.gpuTicksTotal = gpuTicksTotal_;
			result.gpuTicksMinimum = gpuTicksMinimum_;
			result.gpuTicksMaximum = gpuTicksMaximum_;
			result.gpuFrequency = gpuFrequency_;
			result.cpuSamples = cpuSamples_;
			result.cpuRecordNanosecondsTotal = cpuRecordNanosecondsTotal_;
			result.cpuRecordNanosecondsMaximum = cpuRecordNanosecondsMaximum_;
			result.skippedFrames = skippedFrames_;
			result.modelRunShare = modelRunShare_;
			result.frameSamples = frameCount_;
			result.frameQueryFailures = frameQueryFailures_;
			result.frameTicksTotal = frameTicksTotal_;
			result.frameFrequency = frameFrequency_;
			for (std::size_t i = 0; i < count_; ++i) {
				result.newestFirst[i] = points_[(head_ + NeuralTelemetryCapacity - i) % NeuralTelemetryCapacity];
			}
			return result;
		}

	private:
		std::array<std::uint64_t, NeuralTelemetryCapacity> frameTicks_{};
		std::size_t frameHead_{}, frameCount_{};
		std::uint64_t frameQueryFailures_{}, frameTicksTotal_{}, frameFrequency_{};
		void RecordCPU(std::uint64_t a_nanoseconds)
		{
			++cpuSamples_;
			cpuRecordNanosecondsTotal_ += a_nanoseconds;
			cpuRecordNanosecondsMaximum_ = (std::max)(cpuRecordNanosecondsMaximum_, a_nanoseconds);
		}
		void Push(NeuralTelemetryPoint a_point)
		{
			head_ = count_ ? (head_ + 1) % NeuralTelemetryCapacity : 0;
			if (count_ < NeuralTelemetryCapacity) { ++count_; }
			points_[head_] = a_point;
		}

		std::array<NeuralTelemetryPoint, NeuralTelemetryCapacity> points_{};
		std::size_t head_{};
		std::size_t count_{};
		std::uint64_t gpuSamples_{};
		std::uint64_t gpuQueryFailures_{};
		std::uint64_t gpuTicksTotal_{};
		std::uint64_t gpuTicksMinimum_{};
		std::uint64_t gpuTicksMaximum_{};
		std::uint64_t gpuFrequency_{};
		std::uint64_t cpuSamples_{};
		std::uint64_t cpuRecordNanosecondsTotal_{};
		std::uint64_t cpuRecordNanosecondsMaximum_{};
		std::uint64_t skippedFrames_{};
		double modelRunShare_{ 1 };
	};
}
