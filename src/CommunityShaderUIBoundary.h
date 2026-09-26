#pragma once

namespace TheosRenderPipeline
{
    // UI must finish before the producer's display compositor. Present may be
    // reached only after that compositor, or skipped by a suppression hook.
    class CommunityShaderUIBoundary
    {
    public:
        template <class Interface, class Complete>
        void DrawInterface(Interface&& draw, Complete&& complete)
        {
            ++depth_;
            draw();
            if (--depth_ == 0 && !completed_) { completed_ = complete(); }
        }

        template <class Complete>
        void BeforePresent(bool test, bool directFramebuffer, Complete&& complete)
        {
            // A late fallback is safe only when there is no separate UI surface
            // awaiting composition. Do not draw into an already-composited UI.
            if (!test && !completed_ && directFramebuffer) { completed_ = complete(); }
        }

        void AfterPresent(bool test) { if (!test) { completed_ = false; } }

    private:
        unsigned depth_{};
        bool completed_{};
    };
}
