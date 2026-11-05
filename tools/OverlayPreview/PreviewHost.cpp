#include "Localization.h"
#include "OverlayPreview.h"

#include <imgui_impl_dx11.h>

#include <algorithm>

using namespace TheosRenderPipeline::Overlay;

OverlayPreview::OverlayPreview(PreviewScenario& scenario, const Layout& layout, SettingsPage page)
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
    currentLive_ = false;
}

void OverlayPreview::BuildFrame()
{
    // As OnPresent does, around the shared menu code. Offline frames keep
    // requesting the tab while the window settles; live frames request it once.
    currentLive_ = live_;
    if (!live_ || pagePending_)
        menu_->requestedPage = page_;
    pagePending_ = false;
    menu_->menuKeyControlDrawn = false;
    menu_->BuildUI();
    if (!menu_->menuKeyControlDrawn)
        menu_->hotkeys.CancelCapture();
}

void OverlayPreview::UpdateScale()
{
    const auto display = ImGui::GetIO().DisplaySize;
    const float scale = ResolveUIScale(menu_->layout.uiScale, display.x, display.y);
    if (std::abs(scale - UIScale()) < 0.001f)
        return;
    ApplyRendererStyle(scale);
    BuildRendererFont(scale);
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
    menu_->layoutPending = true;
}

bool OverlayPreview::LabMode() const { return menu_->showDeveloperControls; }
void OverlayPreview::SetLabMode(bool enabled) { menu_->showDeveloperControls = enabled; }
bool OverlayPreview::RuntimePresent() const { return menu_->nrRuntimePresent; }
void OverlayPreview::SetRuntimePresent(bool present) { menu_->nrRuntimePresent = present; }
Layout OverlayPreview::CurrentLayout() const { return menu_->layout; }

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

void OverlayUI::ApplySettingsDraft(bool saveAsDefault)
{
    // Live previews treat the draft as applied so the pending-change flow can be
    // tried; the renderer state the menu reads does not follow it.
    if (OverlayPreview::Live())
    {
        OverlayPreview::Scenario().applied = settingsDraft;
        actionMessage = saveAsDefault ? "Preview: saved as the scenario's defaults." : "Preview: applied to the scenario.";
    }
    else
        actionMessage = "Preview only: settings are not applied.";
    actionMessageIsError = false;
}

void OverlayUI::RequestFrameGeneration(bool) {}
void OverlayUI::SetXeFGFlipMetering(bool) {}
// The comparison is live in game; the preview shows the toggle taking effect.
void OverlayUI::SetNeuralCompare(TheosRenderPipeline::NeuralRendering::CompareSettings compare)
{
    OverlayPreview::Scenario().view.neuralApplied.compare = compare;
}
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

void OverlayUI::DrawLabXeFGControls()
{
    bool cadence = false;
    ImGui::BeginDisabled();
    ImGui::Checkbox(TR("XeFG x3/x4 even pacing with VSync (Lab)"), &cadence);
    ImGui::EndDisabled();
    LabDetailsUnavailable();
}

void OverlayUI::DrawLabGenerationControls(const FrameView&)
{
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
