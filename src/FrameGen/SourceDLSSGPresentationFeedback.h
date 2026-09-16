#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace TheosRenderPipeline::SourceDLSSG
{
	enum class PresentationSampleOutcome
	{
		Baseline,
		Unchanged,
		Advanced,
		BatchedAdvance,
		Discontinuity
	};

	// One observed native present-count advance. observedQpc is when the host
	// noticed the count, not a claimed scanout timestamp. syncQpc is optional
	// DXGI feedback for the latest count. A nonzero additionalOutputsInBatch
	// means one host poll observed several native outputs at once. Their separate
	// timestamps are unavailable, but the outputs are not presumed missing.
	struct PresentationFeedbackPoint
	{
		std::uint64_t sequence{};
		std::uint32_t nativeCount{};
		std::uint32_t additionalOutputsInBatch{};
		std::int64_t observedQpc{};
		std::int64_t syncQpc{};
	};

	struct PresentationFeedbackSnapshot
	{
		std::array<PresentationFeedbackPoint, 512> newestFirst{};
		std::size_t count{};
		std::uint64_t epoch{};
		std::uint64_t samples{};
		std::uint64_t observedOutputs{};
		std::uint64_t batchedAdditionalOutputs{};
		std::uint64_t unchangedSamples{};
		std::uint64_t discontinuities{};
		std::uint64_t queryFailures{};
		std::int64_t qpcFrequency{};
		bool hasBaseline{};
	};

	// Pure state model for samples obtained from GetLastPresentCount. It never
	// interpolates missing outputs. Natural uint32 subtraction accepts counter
	// wrap, while implausibly large advances are treated as a new epoch.
	class PresentationFeedbackTracker
	{
	public:
		static constexpr std::size_t Capacity = 512;
		static constexpr std::uint32_t MaximumAdvance = 4096;

		PresentationSampleOutcome Sample(
			std::uint32_t a_presentCount,
			std::int64_t a_observedQpc,
			std::int64_t a_syncQpc = 0)
		{
			++samples_;
			if (a_observedQpc <= 0 || (hasBaseline_ && a_observedQpc < lastObservedQpc_)) {
				BeginEpoch();
				if (a_observedQpc > 0) {
					SetBaseline(a_presentCount, a_observedQpc);
				}
				return PresentationSampleOutcome::Discontinuity;
			}
			if (!hasBaseline_) {
				SetBaseline(a_presentCount, a_observedQpc);
				return PresentationSampleOutcome::Baseline;
			}

			const auto advance = a_presentCount - lastPresentCount_;
			lastObservedQpc_ = a_observedQpc;
			if (!advance) {
				++unchangedSamples_;
				return PresentationSampleOutcome::Unchanged;
			}
			if (advance > MaximumAdvance) {
				BeginEpoch();
				SetBaseline(a_presentCount, a_observedQpc);
				return PresentationSampleOutcome::Discontinuity;
			}

			lastPresentCount_ = a_presentCount;
			observedOutputs_ += advance;
			const auto additional = advance - 1;
			batchedAdditionalOutputs_ += additional;
			Push({ observedOutputs_, a_presentCount, additional, a_observedQpc,
				a_syncQpc > 0 ? a_syncQpc : 0 });
			return additional ? PresentationSampleOutcome::BatchedAdvance : PresentationSampleOutcome::Advanced;
		}

		void BeginEpoch()
		{
			hasBaseline_ = false;
			lastPresentCount_ = 0;
			lastObservedQpc_ = 0;
			++epoch_;
			++discontinuities_;
		}

		const PresentationFeedbackPoint& Newest(std::size_t a_offset = 0) const
		{
			return points_[(head_ + Capacity - a_offset) % Capacity];
		}
		PresentationFeedbackSnapshot Snapshot() const
		{
			PresentationFeedbackSnapshot snapshot{};
			snapshot.count = count_;
			snapshot.epoch = epoch_;
			snapshot.samples = samples_;
			snapshot.observedOutputs = observedOutputs_;
			snapshot.batchedAdditionalOutputs = batchedAdditionalOutputs_;
			snapshot.unchangedSamples = unchangedSamples_;
			snapshot.discontinuities = discontinuities_;
			snapshot.hasBaseline = hasBaseline_;
			for (std::size_t i = 0; i < count_; ++i) {
				snapshot.newestFirst[i] = Newest(i);
			}
			return snapshot;
		}
		std::size_t Count() const { return count_; }
		std::uint64_t Epoch() const { return epoch_; }
		std::uint64_t ObservedOutputs() const { return observedOutputs_; }
		std::uint64_t BatchedAdditionalOutputs() const { return batchedAdditionalOutputs_; }
		std::uint64_t UnchangedSamples() const { return unchangedSamples_; }
		bool HasBaseline() const { return hasBaseline_; }

	private:
		void SetBaseline(std::uint32_t a_presentCount, std::int64_t a_observedQpc)
		{
			lastPresentCount_ = a_presentCount;
			lastObservedQpc_ = a_observedQpc;
			hasBaseline_ = true;
		}

		void Push(PresentationFeedbackPoint a_point)
		{
			head_ = count_ == 0 ? 0 : (head_ + 1) % Capacity;
			if (count_ < Capacity) { ++count_; }
			points_[head_] = a_point;
		}

		std::array<PresentationFeedbackPoint, Capacity> points_{};
		std::size_t head_{};
		std::size_t count_{};
		std::uint64_t epoch_{ 1 };
		std::uint64_t samples_{};
		std::uint64_t observedOutputs_{};
		std::uint64_t batchedAdditionalOutputs_{};
		std::uint64_t unchangedSamples_{};
		std::uint64_t discontinuities_{};
		std::uint32_t lastPresentCount_{};
		std::int64_t lastObservedQpc_{};
		bool hasBaseline_{};
	};
}
