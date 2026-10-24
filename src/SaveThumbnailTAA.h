#pragma once

// Skyrim draws a save's thumbnail (kSAVE_GAME_SCREENSHOT) in its post-processing
// chain, which during gameplay runs only with TAA or depth of field on, and the
// save waits for it. TRP keeps Skyrim's TAA off (DLSS replaces it), so with
// bDoDepthOfField=0 a quicksave waited until a menu's blur ran the chain (Nexus
// report, 0.4.5). SSE Engine Fixes' SaveScreenshots fix does not apply: it checks
// the INI's bUseTAA at startup, which still says on.
//
// While Skyrim asks for a save screenshot, its TAA may run, so the thumbnail is
// drawn and the save goes on; then it is off again. If the request is still set
// kGiveUpFrames frames later, TAA goes back off until the request clears. Only
// during gameplay: a paused game (the journal, the pause menu) asks too, and its
// menu blur draws the thumbnail without TAA.
namespace TheosRenderPipeline
{
    class SaveThumbnailTAA
    {
    public:
        static constexpr unsigned kGiveUpFrames = 8;
        enum class Event { None, Started, Finished, GaveUp };
        struct Decision
        {
            bool taa{};          // let Skyrim's TAA run this frame
            Event event{};
            unsigned frames{};   // frames the request has been set
        };

        // Once per rendered frame, before Skyrim's post-processing runs.
        Decision Next(bool requested)
        {
            if (!requested) {
                const Decision decision{ false, active_ ? Event::Finished : Event::None, frames_ };
                active_ = gaveUp_ = false;
                frames_ = 0;
                return decision;
            }
            ++frames_;
            if (gaveUp_) { return { false, Event::None, frames_ }; }
            if (frames_ > kGiveUpFrames) {
                gaveUp_ = true;
                active_ = false;  // its clearing is not a drawn thumbnail
                return { false, Event::GaveUp, frames_ };
            }
            const auto event = active_ ? Event::None : Event::Started;
            active_ = true;
            return { true, event, frames_ };
        }

    private:
        bool active_{}, gaveUp_{};
        unsigned frames_{};
    };
}
