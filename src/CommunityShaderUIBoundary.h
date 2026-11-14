#pragma once

namespace TheosRenderPipeline
{
    // UI must finish before the producer's display compositor. Present may be
    // reached only after that compositor, or skipped by a suppression hook.
    class CommunityShaderUIBoundary
    {
    public:
        // The engine's world-completion call inside the interface function is
        // hooked. Without it the scene is never read again after the interface
        // starts: by then a direct framebuffer may already hold menus.
        void WorldHookInstalled() { worldHook_ = true; }

        // menusHidden: the engine's own flag, read before the call. scene: the
        // completed scene can be read once more; nothing of the interface is
        // on it. complete: the interface has been drawn.
        template <class Interface, class Scene, class Complete>
        void DrawInterface(bool menusHidden, Interface&& draw, Scene&& scene, Complete&& complete)
        {
            if (depth_++ == 0) { worldSeen_ = false; menusHidden_ = menusHidden; }
            draw();
            if (--depth_ == 0) {
                // With its menus hidden the engine returns before the world-
                // completion call and draws nothing, so the scene is still
                // free of UI here. Entry hooks ran either way. A missing call
                // with menus shown means the hook was bypassed: the scene may
                // hold menus by now and is left alone.
                if (worldHook_ && menusHidden_ && !worldSeen_) { worldSeen_ = true; scene(); }
                if (!completed_) { completed_ = complete(); }
            }
        }

        // From the engine's world-completion call: every hook on the interface
        // entry has run and no menu has been drawn yet.
        template <class Scene>
        void WorldCompleted(Scene&& scene)
        {
            if (depth_ && !worldSeen_) { worldSeen_ = true; scene(); }
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
        bool completed_{}, worldHook_{}, worldSeen_{}, menusHidden_{};
    };
}
