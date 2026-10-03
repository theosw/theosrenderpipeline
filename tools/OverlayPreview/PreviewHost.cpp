#include "OverlayPreview.h"

#include <algorithm>

using namespace TheosRenderPipeline::Overlay;

OverlayPreview::OverlayPreview(const PreviewScenario& scenario, const Layout& layout, SettingsPage page)
    : menu_(new OverlayUI()), page_(page)
{
    current_ = &scenario;
    auto& menu = *menu_;
    menu.initialized = true;
    menu.visible = true;
    menu.layout = layout;
    menu.showDeveloperControls = scenario.labMode;
    menu.nrRuntimePresent = scenario.nrRuntimePresent;
    menu.settingsDraft = scenario.draft;
    menu.settingsDraft.valid = true;
    menu.actionMessage = scenario.actionMessage;
    menu.actionMessageIsError = scenario.actionMessageIsError;
    const auto& presets = scenario.draft.appearance.presets;
    if (scenario.selectedPreset >= 0 && scenario.selectedPreset < static_cast<int>(presets.size()))
    {
        menu.presetSelected = presets[scenario.selectedPreset].id;
    }
    const auto count = (std::min)(scenario.frameTimesMs.size(), static_cast<std::size_t>(OverlayUI::kFrameHistory));
    std::copy(scenario.frameTimesMs.end() - static_cast<std::ptrdiff_t>(count), scenario.frameTimesMs.end(),
        menu.frameTimesMs);
    menu.frameTimeCount = static_cast<int>(count);
    menu.frameTimeIndex = static_cast<int>(count % OverlayUI::kFrameHistory);
    menu.renderedFps = scenario.renderedFps;
    menu.presentedFps = scenario.view.outputRate.available ? scenario.view.outputRate.fps : scenario.renderedFps;
}

OverlayPreview::~OverlayPreview()
{
    current_ = nullptr;
}

void OverlayPreview::BuildFrame()
{
    // As OnPresent does, around the shared menu code.
    menu_->requestedPage = page_;
    menu_->menuKeyControlDrawn = false;
    menu_->BuildUI();
}

// The renderer-facing OverlayUI members. In the plugin they read or change the
// renderer (OverlayUI.cpp, OverlayFrameCapture.cpp and OverlayLabPanel.cpp);
// here they return the scenario and leave everything else unchanged.

OverlayUI::FrameView OverlayUI::CaptureFrameView()
{
    auto view = OverlayPreview::Scenario().view;
    DescribeFrameView(view);
    return view;
}

void OverlayUI::CaptureSettingsDraft()
{
    hotkeys.CancelCapture();
    menuKeyError.clear();
    settingsDraft = OverlayPreview::Scenario().applied;
    settingsDraft.valid = true;
}

int OverlayUI::CountStagedChanges() const
{
    auto applied = OverlayPreview::Scenario().applied;
    applied.valid = true;
    return TheosRenderPipeline::CountRendererSettingsChanges(settingsDraft, applied);
}

void OverlayUI::ApplySettingsDraft(bool)
{
    actionMessage = "Preview only: settings are not applied.";
    actionMessageIsError = false;
}

void OverlayUI::RequestFrameGeneration(bool) {}
void OverlayUI::PausePresets(bool) {}
void OverlayUI::RefreshWeatherList() {}
void OverlayUI::ResetSessionFallbacks() {}
void OverlayUI::ClearTimingWindow() {}

namespace
{
void LabDetailsUnavailable()
{
    ImGui::TextDisabled("Lab runtime details are drawn only in game.");
}
} // namespace

void OverlayUI::DrawLabImageDetails(const FrameView&)
{
    ImGui::Separator();
    LabDetailsUnavailable();
}

void OverlayUI::DrawLabGenerationDetails(const FrameView&)
{
    LabDetailsUnavailable();
}

void OverlayUI::DrawLabMenuDiagnostics()
{
    LabDetailsUnavailable();
}
