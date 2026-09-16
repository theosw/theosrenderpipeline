#pragma once

#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace TheosRenderPipeline::Telemetry
{
	enum class OutputSource { Unavailable, DXGI, Streamline };

	struct OutputCounter
	{
		OutputSource source{ OutputSource::Unavailable };
		std::uint64_t epoch{}, observations{}, frames{};
		bool available{};
	};

	struct OutputRate
	{
		OutputSource source{ OutputSource::Unavailable };
		float fps{};
		bool available{};
	};

	// Count producer-reported output, never raster FPS * a requested multiplier.
	// The consumer reads cached totals, not an additional consuming runtime query.
	// This is a runtime presentation rate, NOT physical scanout/cadence evidence.
	class OutputRateSampler
	{
	public:
		void Update(double a_nowMs, const OutputCounter& a_counter, bool a_discontinuity = false)
		{
			if (!a_counter.available || a_counter.source == OutputSource::Unavailable || !std::isfinite(a_nowMs)) {
				*this = {};
				return;
			}
			if (!initialized_ || a_discontinuity || a_counter.source != previous_.source ||
				a_counter.epoch != previous_.epoch || a_counter.frames < previous_.frames ||
				a_counter.observations < previous_.observations || a_nowMs < lastPollMs_ ||
				a_nowMs - lastPollMs_ > 250.0) {
				Baseline(a_nowMs, a_counter);
				return;
			}
			if (a_counter.observations != previous_.observations) { lastObservationMs_ = a_nowMs; }
			if (a_nowMs - lastObservationMs_ > 250.0) {
				Baseline(a_nowMs, a_counter);
				return;
			}
			previous_ = a_counter;
			lastPollMs_ = a_nowMs;
			const auto elapsed = a_nowMs - windowMs_;
			if (elapsed >= 1000.0) {
				rate_ = { a_counter.source,
					static_cast<float>((a_counter.frames - windowFrames_) * 1000.0 / elapsed), true };
				windowMs_ = a_nowMs;
				windowFrames_ = a_counter.frames;
			}
		}
		const OutputRate& Rate() const { return rate_; }
	private:
		void Baseline(double a_nowMs, const OutputCounter& a_counter)
		{
			initialized_ = true;
			previous_ = a_counter;
			lastPollMs_ = lastObservationMs_ = windowMs_ = a_nowMs;
			windowFrames_ = a_counter.frames;
			rate_ = { a_counter.source, 0.0f, false };
		}
		bool initialized_{};
		OutputCounter previous_{};
		OutputRate rate_{};
		double lastPollMs_{}, lastObservationMs_{}, windowMs_{};
		std::uint64_t windowFrames_{};
	};

	inline std::string Milliseconds(float a_value, bool a_available)
	{
		return a_available && std::isfinite(a_value) && a_value >= 0.0f ?
			std::format("{:.3f}", a_value) : "unavailable";
	}
}
