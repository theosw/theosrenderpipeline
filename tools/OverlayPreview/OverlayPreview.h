#pragma once

#include "OverlayFrameView.h"
#include "OverlayLayout.h"
#include "OverlayPipeline.h"
#include "RendererSettings.h"

#include <memory>
#include <string>
#include <vector>

// One menu state to draw offline: the renderer snapshot the menu reads, plus
// the menu's own state. The preview never touches the renderer or the game.
struct PreviewScenario
{
    std::string name;
    std::string summary;
    OverlayUI::FrameView view;
    // Settings currently applied, and the draft the menu edits. They differ
    // when the scenario shows unapplied changes.
    TheosRenderPipeline::RendererSettingsDraft applied;
    TheosRenderPipeline::RendererSettingsDraft draft;
    // Game-facing Present intervals, oldest first; at most 120 are shown.
    std::vector<float> frameTimesMs;
    float renderedFps{};
    bool nrRuntimePresent{true};
    bool labMode{};
    // Preset opened in the editor, by position in draft.appearance.presets; -1 opens Base.
    int selectedPreset{-1};
    std::string actionMessage;
    bool actionMessageIsError{};
};

std::vector<PreviewScenario> PreviewScenarios();

// Draws the production menu (OverlayMenu.cpp and the panels) for a scenario.
// A friend of OverlayUI; PreviewHost.cpp supplies the renderer-facing members.
class OverlayPreview
{
public:
    OverlayPreview(const PreviewScenario& scenario, const TheosRenderPipeline::Overlay::Layout& layout,
        TheosRenderPipeline::Overlay::SettingsPage page);
    ~OverlayPreview();
    OverlayPreview(const OverlayPreview&) = delete;
    OverlayPreview& operator=(const OverlayPreview&) = delete;

    // Builds one ImGui frame of the menu. Call between NewFrame and Render.
    void BuildFrame();

    // The scenario being drawn, for PreviewHost.cpp.
    static const PreviewScenario& Scenario() { return *current_; }

private:
    std::unique_ptr<OverlayUI> menu_;
    TheosRenderPipeline::Overlay::SettingsPage page_;
    static inline const PreviewScenario* current_{};
};
