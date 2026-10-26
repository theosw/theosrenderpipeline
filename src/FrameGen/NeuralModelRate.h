#pragma once
#include <atomic>
#include <cstdint>

// Model rate: run NR's model on every frame, or on every second or third frame and
// carry its change to the frames between with motion vectors and depth. The approach
// follows cavy8's Alternate Frames for Community Shaders NR (Bottled Shaders
// PR #6): evaluate on alternate frames, reproject the previous result with
// motion, depth and material category, compose two frames of motion for the
// model's history, and evaluate whenever the chain is broken.
namespace TheosRenderPipeline::NeuralRendering
{
	// Saved settings hold EveryFrame or EveryOtherFrame; EveryThirdFrame is Lab only
	// (LabEveryThirdFrame) and never saved.
	enum class ModelRate : std::uint32_t { EveryFrame, EveryOtherFrame, EveryThirdFrame };
	// A saved rate: every frame or every other frame.
	inline ModelRate SanitizeModelRate(ModelRate value)
	{
		return value == ModelRate::EveryOtherFrame ? value : ModelRate::EveryFrame;
	}
	// Lab, for this session only: Every other frame runs the model on every third
	// frame instead. Off at every start.
	inline std::atomic_bool& LabEveryThirdFrame()
	{
		static std::atomic_bool value{};
		return value;
	}
	// The rate the schedule runs: the saved rate, or every third frame for Every
	// other frame while the Lab switch is on.
	inline ModelRate EffectiveModelRate(ModelRate saved, bool labEveryThirdFrame)
	{
		const auto rate = SanitizeModelRate(saved);
		return rate == ModelRate::EveryOtherFrame && labEveryThirdFrame ? ModelRate::EveryThirdFrame : rate;
	}
	inline ModelRate EffectiveModelRate(ModelRate saved) { return EffectiveModelRate(saved, LabEveryThirdFrame()); }
	// Frames per model run: 1, 2 or 3.
	inline unsigned ModelRateInterval(ModelRate value)
	{
		return value == ModelRate::EveryThirdFrame ? 3u : value == ModelRate::EveryOtherFrame ? 2u : 1u;
	}

	// One frame's facts, sampled by the NR pass before it records anything.
	struct ModelRateFrame
	{
		ModelRate rate{};
		// NR history restarts this frame (camera cut, settings, freeze released,
		// object controls switching, a new feature).
		bool reset{};
		// The stored change or guides cannot stand for this frame: a setting that
		// changes NR's output, the passes that run, a pass created this frame, a
		// gap since the previous frame.
		bool changed{};
		// Compare > Freeze frame: the held frame is evaluated every frame.
		bool frozen{};
	};

	struct ModelRateDecision
	{
		bool evaluate{ true };
		// The model's history is older than one frame: give it this frame's motion
		// composed with the skipped frames' (accumulated in the previous guides).
		bool composeMotion{};
		// A skip after a skip (Every third frame): after reprojecting, write the
		// motion back to the last evaluation into this frame's guides.
		bool accumulateMotion{};
		// Keep this frame's motion, depth and object group for the next frame.
		bool captureGuides{};
		// Keep this frame's change (evaluated, or carried onto it) for the next frame.
		bool storeChange{};
		bool operator==(const ModelRateDecision&) const = default;
	};

	// A frame may skip the model only when the frame immediately before it stored
	// its change and nothing since has invalidated it, and at most interval-1
	// frames in a row skip. Every third frame chains: the first skipped frame
	// stores the change it carried, and the second carries that one frame on
	// (as the 0.5.0 Lab detail reuse did), so each reprojection moves one frame.
	// Next is called once per recorded frame; a failed recording ends the pass,
	// so there is no rollback.
	class ModelRateSchedule
	{
	public:
		ModelRateDecision Next(const ModelRateFrame& frame)
		{
			ModelRateDecision decision;
			const unsigned interval = frame.frozen ? 1u : ModelRateInterval(frame.rate);
			const bool alternate = interval > 1;
			decision.evaluate = !(alternate && stored_ && !frame.reset && !frame.changed && skipped_ + 1 < interval);
			// Composition also follows a switch back to every frame: the history
			// is still the frame before the skipped ones. A reset has no history.
			decision.composeMotion = decision.evaluate && skipped_ && !frame.reset;
			decision.accumulateMotion = !decision.evaluate && skipped_ > 0;
			decision.captureGuides = alternate || decision.composeMotion;
			const unsigned run = decision.evaluate ? 0u : skipped_ + 1;
			// Store whenever the next frame may skip: after an evaluation, and after
			// a skip that leaves room for another.
			decision.storeChange = alternate && run + 1 < interval;
			stored_ = decision.storeChange;
			skipped_ = run;
			return decision;
		}
		// Forget the stored frame, for example after a recording failure.
		void Invalidate() { stored_ = false; skipped_ = 0; }
	private:
		bool stored_{};
		unsigned skipped_{};  // frames skipped in a row up to the last one
	};
}
