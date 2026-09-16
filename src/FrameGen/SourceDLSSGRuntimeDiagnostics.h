#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace TheosRenderPipeline::SourceDLSSG
{
	enum class RuntimeDiagnosticEvent : std::size_t
	{
		PacerSkip,
		PresentSkip,
		DroppedOutput,
		IdentityMismatch,
		WarmupReset,
		SynchronizationFailure,
		Count
	};

	constexpr std::optional<RuntimeDiagnosticEvent> ClassifyRuntimeMessage(std::string_view a_message)
	{
		using Event = RuntimeDiagnosticEvent;
		if (a_message.find("CPU pacer is skipping the frame") != std::string_view::npos ||
			a_message.find("Skipping the dlfg frame") != std::string_view::npos) {
			return Event::PacerSkip;
		}
		if (a_message.find("Skipping present") != std::string_view::npos ||
			a_message.find("skip the present") != std::string_view::npos ||
			a_message.find("Skipping the present") != std::string_view::npos ||
			a_message.find("Out of order frame - will skip the present") != std::string_view::npos) {
			return Event::PresentSkip;
		}
		if (a_message.find("Dropped-present counter underflow") != std::string_view::npos ||
			a_message.find("Unrequested drop for frame") != std::string_view::npos) {
			return Event::DroppedOutput;
		}
		if (a_message.find("Rendered ID mismatch") != std::string_view::npos ||
			a_message.find("Present ID mismatch") != std::string_view::npos ||
			a_message.find("Dropped ID mismatch") != std::string_view::npos) {
			return Event::IdentityMismatch;
		}
		if (a_message.find("invalidating warmup") != std::string_view::npos ||
			a_message.find("not working during warmup") != std::string_view::npos ||
			a_message.find("State changed during warmup") != std::string_view::npos) {
			return Event::WarmupReset;
		}
		if (a_message.find("setReflexTiming NvAPI call failed") != std::string_view::npos ||
			a_message.find("RSYNC: Fence wait error") != std::string_view::npos ||
			a_message.find("Tracker init failed") != std::string_view::npos ||
			a_message.find("ctx, fence, or compute is null, aborting") != std::string_view::npos) {
			return Event::SynchronizationFailure;
		}
		return std::nullopt;
	}

	struct RuntimeDiagnosticsSnapshot
	{
		std::array<std::uint64_t, static_cast<std::size_t>(RuntimeDiagnosticEvent::Count)> counts{};

		std::uint64_t Count(RuntimeDiagnosticEvent a_event) const
		{
			return counts[static_cast<std::size_t>(a_event)];
		}
	};

	class RuntimeDiagnostics
	{
	public:
		void Record(std::string_view a_message)
		{
			if (const auto event = ClassifyRuntimeMessage(a_message)) {
				counts_[static_cast<std::size_t>(*event)].fetch_add(1, std::memory_order_relaxed);
			}
		}

		RuntimeDiagnosticsSnapshot Snapshot() const
		{
			RuntimeDiagnosticsSnapshot snapshot{};
			for (std::size_t i = 0; i < snapshot.counts.size(); ++i) {
				snapshot.counts[i] = counts_[i].load(std::memory_order_relaxed);
			}
			return snapshot;
		}

	private:
		std::array<std::atomic<std::uint64_t>,
			static_cast<std::size_t>(RuntimeDiagnosticEvent::Count)> counts_{};
	};
}
