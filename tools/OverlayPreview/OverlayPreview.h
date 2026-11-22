#pragma once

#include "OverlayFrameView.h"
#include "OverlayLayout.h"
#include "OverlayPipeline.h"
#include "RendererSettings.h"

#include <d3d11.h>
#include <imgui_internal.h>

#include <filesystem>
#include <memory>
#include <optional>
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

// The scenario as it would run at this output size, keeping its render scale.
PreviewScenario AtOutputSize(PreviewScenario scenario, UINT width, UINT height);

struct PreviewImage
{
    UINT width{}, height{};
    std::vector<std::uint8_t> bgr;
};

// Copies a region of an RGBA8 texture to BGR rows for ScreenshotFile. An empty
// rect copies the whole texture.
std::optional<PreviewImage> ReadPreviewTexture(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* texture, const ImRect& rect);

struct LivePreviewOptions
{
    std::string scenario;
    UINT width{1920}, height{1080};
    std::optional<TheosRenderPipeline::Overlay::Layout> layout;
    std::filesystem::path out{"out/overlay-preview"};
    bool warp{};
    // A scripted run: positions relative to the menu window's top-left, in
    // output pixels. Each click is followed by a short wait; the hover stays
    // long enough for tooltips. The run then saves the menu to save and exits.
    // A typing step sends text one character per frame at the last position;
    // "{enter}" presses Enter.
    struct Step
    {
        float x{}, y{};
        bool click{};
        std::string text;
    };
    std::vector<Step> script;
    std::filesystem::path save;
    bool full{};
    // As [Overlay] Language: auto, en or a dictionary code.
    std::string language{"auto"};
    // The tab the menu opens on, from --tab.
    TheosRenderPipeline::Overlay::SettingsPage page{TheosRenderPipeline::Overlay::SettingsPage::None};
};

// The interactive window (LivePreview.cpp). Returns the process exit code.
int RunLivePreview(const LivePreviewOptions& options);

// Draws the production menu (OverlayMenu.cpp and the panels) for a scenario.
// A friend of OverlayUI; PreviewHost.cpp supplies the renderer-facing members.
class OverlayPreview
{
public:
    // The scenario must outlive the preview; the live window edits it while drawing.
    OverlayPreview(PreviewScenario& scenario, const TheosRenderPipeline::Overlay::Layout& layout,
        TheosRenderPipeline::Overlay::SettingsPage page);
    ~OverlayPreview();
    OverlayPreview(const OverlayPreview&) = delete;
    OverlayPreview& operator=(const OverlayPreview&) = delete;

    // Builds one ImGui frame of the menu. Call between NewFrame and Render.
    void BuildFrame();

    // Live mode selects the starting tab once and then leaves tabs to the user,
    // and Apply updates the scenario's applied settings.
    void SetLive(bool live) { live_ = live; }
    static bool Live() { return current_ && currentLive_; }
    // As OverlayUI::UpdateUIScale: rebuild the style, font and font texture when
    // the zoom or output size changes. Call before NewFrame.
    void UpdateScale();
    bool LabMode() const;
    void SetLabMode(bool enabled);
    bool RuntimePresent() const;
    void SetRuntimePresent(bool present);
    TheosRenderPipeline::Overlay::Layout CurrentLayout() const;
    // The Copy details text for the scenario, with the preview's invented system part.
    std::string SupportText();

    // The scenario being drawn, for PreviewHost.cpp.
    static PreviewScenario& Scenario() { return *current_; }

private:
    std::unique_ptr<OverlayUI> menu_;
    TheosRenderPipeline::Overlay::SettingsPage page_;
    bool live_{};
    bool pagePending_{true};
    static inline PreviewScenario* current_{};
    static inline bool currentLive_{};
};
