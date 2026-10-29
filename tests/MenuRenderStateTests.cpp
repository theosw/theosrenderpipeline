#include "MenuRenderState.h"
#include "FrameGen/SceneReset.h"
#include "FrameGen/SourceGenerationPolicy.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using TheosRenderPipeline::MenuRenderBoundary;
using TheosRenderPipeline::MenuRenderState;
using TheosRenderPipeline::SceneResetTracker;
using TheosRenderPipeline::SourceGenerationEnabled;
using enum MenuRenderBoundary;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static bool CanGenerate(const MenuRenderState& menus)
{
    return SourceGenerationEnabled(0, true, true, menus.SuspendGeneration());
}

static void CharacterCreationAfterLoading()
{
    MenuRenderState menus;
    // Replay startup and new-character menu ordering, including the overlap
    // that formerly left FG/NR and jitter suspended for all of character editing.
    menus.OnEvent(Loading, true);
    menus.OnEvent(Fader, true);
    menus.OnEvent(Main, true);
    Require(!menus.OnEvent(Loading, false), "startup loading close retains main-menu history suspension");
    Require(menus.SuspendJitter() && !CanGenerate(menus), "Main survives overlapping Loading close");
    menus.OnEvent(Fader, false);
    menus.OnEvent(Main, false);
    Require(menus.SuspendJitter(), "Main close keeps jitter suspended until the next load completes");
    menus.OnEvent(Loading, true);
    menus.OnEvent(Fader, true);
    Require(menus.OnEvent(Loading, false), "world entry resets temporal history");
    Require(!menus.SuspendJitter() && !CanGenerate(menus), "Fader still blocks FG/NR after world entry");
    Require(menus.OnEvent(Character, true), "character entry resets camera and appearance history");
    Require(!menus.SuspendJitter() && !CanGenerate(menus), "character entry preserves jitter and the active fade guard");
    Require(menus.OnEvent(Fader, false), "fade completion resets history");
    Require(CanGenerate(menus) && !menus.SuspendJitter(), "settled character editing admits FG/NR and temporal jitter");
    Require(!SourceGenerationEnabled(1, true, true, menus.SuspendGeneration()), "host warm-up still applies in character editing");
    Require(!SourceGenerationEnabled(0, false, true, menus.SuspendGeneration()), "character editing respects FG opt-out");
    Require(!SourceGenerationEnabled(0, true, false, menus.SuspendGeneration()), "character editing still requires prepared inputs");
    Require(menus.OnEvent(Character, false), "character exit resets history");
    Require(CanGenerate(menus) && !menus.SuspendJitter(), "character exit keeps gameplay eligible");
}

static void ReopenCharacterEditor()
{
    MenuRenderState menus;
    for (int visit = 0; visit < 2; ++visit) {
        Require(menus.OnEvent(Character, true), "reopening the editor resets history");
        Require(CanGenerate(menus) && !menus.SuspendJitter(), "an in-world editor does not latch off rendering");
        Require(menus.OnEvent(Character, false), "returning to the world resets history");
        Require(CanGenerate(menus) && !menus.SuspendJitter(), "return to gameplay preserves rendering");
    }
}

static void LoadingOverCharacterEditor(bool fadeClosesFirst, bool editorClosesDuringLoad)
{
    MenuRenderState menus;
    menus.OnEvent(Character, true);
    menus.OnEvent(Loading, true);
    menus.OnEvent(Fader, true);
    if (editorClosesDuringLoad) {
        Require(menus.OnEvent(Character, false), "editor exit requests history reset even during loading");
    }
    Require(!CanGenerate(menus) && menus.SuspendJitter(), "loading wins over either character-editor state");
    const auto first = fadeClosesFirst ? Fader : Loading;
    const auto last = fadeClosesFirst ? Loading : Fader;
    menus.OnEvent(first, false);
    Require(!CanGenerate(menus), "one closing boundary cannot bypass the other");
    Require(menus.OnEvent(last, false), "the final loading/fade close resets history");
    Require(CanGenerate(menus) && !menus.SuspendJitter(), "either close order returns to a renderable scene");
}

static void MapIsACameraCut(bool fastTravel)
{
    MenuRenderState menus;
    Require(menus.OnEvent(Map, true), "opening the map resets history");
    Require(CanGenerate(menus) && !menus.SuspendJitter(), "the map keeps jitter, FG and NR");
    Require(menus.OnEvent(Map, false), "closing the map resets history");
    Require(CanGenerate(menus) && !menus.SuspendJitter(), "closing the map returns to normal gameplay");
    if (fastTravel) {
        menus.OnEvent(Loading, true);
        menus.OnEvent(Fader, true);
        Require(!CanGenerate(menus) && menus.SuspendJitter(), "fast travel's load still suspends rendering");
        Require(menus.OnEvent(Loading, false), "fast travel's world entry resets history");
        Require(menus.OnEvent(Fader, false) && CanGenerate(menus), "fast travel's fade completion resets and admits FG");
    }
}

// Requests come from menu events on the main thread; one tracker per temporal
// consumer settles them on the render thread.
static void SceneResetWaitsForTheWorld()
{
    SceneResetTracker tracker;
    std::uint32_t generation = 0;
    Require(!tracker.Begin(generation, true) && !tracker.Succeeded(), "no request, no reset");
    ++generation;  // save loaded while the loading screen still shows
    for (int frame = 0; frame < 40; ++frame) {
        Require(!tracker.Begin(generation, false), "loading-screen frames keep their history");
        Require(!tracker.Succeeded(), "loading-screen frames do not settle the world's reset");
    }
    ++generation;  // loading screen closed: coalesces with the pending request
    Require(tracker.Begin(generation, true), "the first world frame resets");
    Require(tracker.Begin(generation, true), "a failed world frame leaves the reset owed");
    const auto settled = tracker.Succeeded();
    Require(settled && settled->generation == 2 && settled->resetFrames == 2 && settled->deferredFrames == 40,
        "success settles the newest generation and reports the frames it took");
    for (int frame = 0; frame < 30; ++frame) {
        Require(!tracker.Begin(generation, true) && !tracker.Succeeded(), "later world frames keep their history");
    }
}

static void SceneResetDuringAFrame()
{
    SceneResetTracker tracker;
    Require(tracker.Begin(1, true), "map opened resets the next frame");
    // Map closes while that frame is being evaluated: the frame settles only
    // the generation it started with.
    Require(tracker.Succeeded()->generation == 1, "the frame settles the generation it began with");
    Require(tracker.Begin(2, true), "a request during the frame resets the following one");
    Require(tracker.Succeeded()->generation == 2, "then settles it");
}

static void SceneResetPerConsumer()
{
    SceneResetTracker upscaler, communityShaders;
    Require(upscaler.Begin(1, true) && upscaler.Succeeded(), "the upscaler settles its own reset");
    Require(communityShaders.Begin(1, false) == false, "a consumer still behind the loading screen keeps its reset");
    Require(communityShaders.Begin(1, true), "and applies it on its own first world frame");
    Require(!upscaler.Begin(1, true), "one consumer's success does not reset another again");
}

int main()
{
    CharacterCreationAfterLoading();
    ReopenCharacterEditor();
    for (bool fadeClosesFirst : {false, true}) {
        for (bool editorClosesDuringLoad : {false, true}) {
            LoadingOverCharacterEditor(fadeClosesFirst, editorClosesDuringLoad);
        }
    }
    MapIsACameraCut(false);
    MapIsACameraCut(true);
    SceneResetWaitsForTheWorld();
    SceneResetDuringAFrame();
    SceneResetPerConsumer();
    std::puts("Menu rendering transition scenarios passed");
}
