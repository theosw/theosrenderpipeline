#pragma once

namespace TheosRenderPipeline
{
    enum class MenuRenderBoundary { Main, Loading, Character, Fader };

    // Menu events overlap: Loading can close after Main opens, and character
    // creation can open before the load-to-world fade finishes.
    class MenuRenderState
    {
    public:
        // Returns whether the next rendered frames need fresh temporal history.
        bool OnEvent(MenuRenderBoundary menu, bool opening)
        {
            switch (menu) {
            case MenuRenderBoundary::Main:
                mainOpen_ = opening;
                // Closing Main precedes a load or quit. Keep jitter suspended
                // through the gap until Loading closes, as before.
                if (opening) { suspendJitter_ = true; }
                return false;
            case MenuRenderBoundary::Loading:
                loadingOpen_ = opening;
                if (opening) { suspendJitter_ = true; }
                else if (!mainOpen_) {
                    suspendJitter_ = false;
                    return true;
                }
                return false;
            case MenuRenderBoundary::Character:
                // Character editing is a rendered scene, not a load boundary.
                // Reset its camera/appearance history on entry and exit without
                // suppressing DLSS jitter, FG or NR for the whole menu session.
                return true;
            case MenuRenderBoundary::Fader:
                faderOpen_ = opening;
                return !opening && !suspendJitter_;
            }
            return false;
        }

        bool SuspendGeneration() const { return mainOpen_ || loadingOpen_ || faderOpen_; }
        bool SuspendJitter() const { return suspendJitter_; }

    private:
        bool mainOpen_{}, loadingOpen_{}, faderOpen_{}, suspendJitter_{};
    };
}
