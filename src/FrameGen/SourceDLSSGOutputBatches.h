#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace TheosRenderPipeline::SourceDLSSG
{
	constexpr std::size_t OutputBatchCapacity = 128;

	enum class OutputBatchMode
	{
		Off,
		Fixed,
		Dynamic
	};

	struct OutputBatchConfiguration
	{
		OutputBatchMode mode{ OutputBatchMode::Off };
		std::uint32_t configuredGeneratedFrames{ 1 };
		std::uint32_t dynamicTargetFPS{};
		bool operator==(const OutputBatchConfiguration&) const = default;
	};

	struct OutputBatchPoint
	{
		std::uint64_t hostFrameID{};
		std::uint64_t epoch{};
		OutputBatchConfiguration configuration{};
		std::uint32_t actualPresented{};
		std::uint32_t realPresented{};
		std::uint32_t generatedPresented{};
		std::uint32_t interpolatedNotPresented{};
		bool countKnown{};
		bool countValid{};
		bool configurationChanged{};
	};

	struct OutputBatchSnapshot
	{
		std::array<OutputBatchPoint, OutputBatchCapacity> newestFirst{};
		std::size_t count{};
		std::uint64_t epoch{};
		std::uint64_t hostPresents{};
		std::uint64_t validBatches{};
		std::uint64_t invalidBatches{};
		std::uint64_t unknownBatches{};
		std::uint64_t actualOutputs{};
		std::uint64_t realOutputs{};
		std::uint64_t generatedOutputs{};
		std::uint64_t interpolatedNotPresented{};
		std::uint64_t configurationChanges{};
		std::uint64_t fixedCountMismatches{};
	};

	// One record per successful host Present and its immediately following
	// slDLSSGGetState read. Fixed mode has a known requested output count.
	// Dynamic mode deliberately does not infer a requested generated count.
	class OutputBatchTracker
	{
	public:
		OutputBatchPoint Record(std::uint64_t a_hostFrameID,
			OutputBatchConfiguration a_configuration, std::optional<std::uint32_t> a_actualPresented)
		{
			OutputBatchPoint point{};
			point.hostFrameID = a_hostFrameID;
			point.epoch = epoch_;
			point.configuration = a_configuration;
			point.countKnown = a_actualPresented.has_value();
			point.actualPresented = a_actualPresented.value_or(0);
			point.configurationChanged = hasConfiguration_ && a_configuration != lastConfiguration_;
			if (point.configurationChanged) { ++configurationChanges_; }
			lastConfiguration_ = a_configuration;
			hasConfiguration_ = true;
			++hostPresents_;

			if (point.actualPresented) {
				point.countValid = true;
				point.realPresented = 1;
				point.generatedPresented = point.actualPresented - 1;
				if (a_configuration.mode == OutputBatchMode::Off) {
					point.countValid = point.actualPresented == 1;
				} else if (a_configuration.mode == OutputBatchMode::Fixed) {
					const auto expected = a_configuration.configuredGeneratedFrames + 1;
					point.countValid = point.actualPresented <= expected;
					if (point.countValid) {
						point.interpolatedNotPresented = expected - point.actualPresented;
					}
				}
			}

			if (!point.countKnown) {
				++unknownBatches_;
			} else if (point.countValid) {
				++validBatches_;
				actualOutputs_ += point.actualPresented;
				realOutputs_ += point.realPresented;
				generatedOutputs_ += point.generatedPresented;
				interpolatedNotPresented_ += point.interpolatedNotPresented;
			} else {
				++invalidBatches_;
				if (a_configuration.mode != OutputBatchMode::Dynamic) { ++fixedCountMismatches_; }
			}
			Push(point);
			return point;
		}

		void BeginEpoch()
		{
			++epoch_;
			hasConfiguration_ = false;
			lastConfiguration_ = {};
		}

		OutputBatchSnapshot Snapshot() const
		{
			OutputBatchSnapshot snapshot{};
			snapshot.count = count_;
			snapshot.epoch = epoch_;
			snapshot.hostPresents = hostPresents_;
			snapshot.validBatches = validBatches_;
			snapshot.invalidBatches = invalidBatches_;
			snapshot.unknownBatches = unknownBatches_;
			snapshot.actualOutputs = actualOutputs_;
			snapshot.realOutputs = realOutputs_;
			snapshot.generatedOutputs = generatedOutputs_;
			snapshot.interpolatedNotPresented = interpolatedNotPresented_;
			snapshot.configurationChanges = configurationChanges_;
			snapshot.fixedCountMismatches = fixedCountMismatches_;
			for (std::size_t i = 0; i < count_; ++i) { snapshot.newestFirst[i] = Newest(i); }
			return snapshot;
		}

		const OutputBatchPoint& Newest(std::size_t a_offset = 0) const
		{
			return points_[(head_ + OutputBatchCapacity - a_offset) % OutputBatchCapacity];
		}

	private:
		void Push(OutputBatchPoint a_point)
		{
			head_ = count_ == 0 ? 0 : (head_ + 1) % OutputBatchCapacity;
			if (count_ < OutputBatchCapacity) { ++count_; }
			points_[head_] = a_point;
		}

		std::array<OutputBatchPoint, OutputBatchCapacity> points_{};
		std::size_t head_{};
		std::size_t count_{};
		std::uint64_t epoch_{ 1 };
		std::uint64_t hostPresents_{};
		std::uint64_t validBatches_{};
		std::uint64_t invalidBatches_{};
		std::uint64_t unknownBatches_{};
		std::uint64_t actualOutputs_{};
		std::uint64_t realOutputs_{};
		std::uint64_t generatedOutputs_{};
		std::uint64_t interpolatedNotPresented_{};
		std::uint64_t configurationChanges_{};
		std::uint64_t fixedCountMismatches_{};
		OutputBatchConfiguration lastConfiguration_{};
		bool hasConfiguration_{};
	};
}
