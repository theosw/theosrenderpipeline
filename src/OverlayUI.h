#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <functional>

#include "TextureProviderBridge.h"
#include "FrameGen/SourceDLSSGSettings.h"
#include "FrameTelemetry.h"
#include "OverlayNumericInput.h"
#include "OverlayHotkeys.h"
#include "RendererSettings.h"
#include "OverlayPipeline.h"
#include "OverlayLayout.h"

namespace TheosRenderPipeline::SourceDLSSG { struct NeuralSnapshot; }

// In-game ImGui overlay: upscaler stats (rendered vs presented FPS, NGX eval
// results) and live controls. Toggled with the ToggleOverlay hotkey (default
// END). Rendered from the Present hook on the render thread.
class OverlayUI
{
public:
	static OverlayUI* GetSingleton()
	{
		static OverlayUI overlay;
		return &overlay;
	}

	void Init(IDXGISwapChain* a_swapChain, ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	// Called from the Present hook, before the original Present executes.
    void OnPresent(ID3D11Texture2D* producerUI = nullptr);

    void ObserveGameHotkeys(const std::vector<UINT>& pressedKeys) { hotkeys.ObserveGameKeys(pressedKeys); }

    // The renderer state the menu draws from (OverlayFrameView.h).
    struct FrameView;

private:
	OverlayUI() = default;
	OverlayUI(const OverlayUI&) = delete;
	OverlayUI& operator=(const OverlayUI&) = delete;
    // Draws the menu offline from scenarios (tools/OverlayPreview).
    friend class OverlayPreview;

    // Drawing (OverlayMenu.cpp, OverlayFrameView.cpp and the panels) reads the
    // renderer only through FrameView and changes it only through the members
    // below, so the same menu code also draws without the game.
    // Defined in OverlayFrameCapture.cpp, OverlayUI.cpp and OverlayLabPanel.cpp:
    FrameView CaptureFrameView();
    void CaptureSettingsDraft();
    int CountStagedChanges() const;
    void ApplySettingsDraft(bool a_saveAsDefault);
    void RequestFrameGeneration(bool enabled);
    void PausePresets(bool paused);
    void RefreshWeatherList();
    void ResetSessionFallbacks();
    void ClearTimingWindow();
    void DrawLabImageDetails(const FrameView& view);
    void DrawLabGenerationDetails(const FrameView& view);
    void DrawLabMenuDiagnostics();

    void DescribeFrameView(FrameView& view) const;
    void DrawPipelineSummary(const FrameView& view);
    void DrawImagePanel(float tabCardHeight, const FrameView& view);
    bool PresetEditorSelected() const;
    void DrawPresetList(const FrameView& view);
    void DrawPresetEditor(const FrameView& view,
        const std::function<void(TheosRenderPipeline::SourceDLSSG::Preferences&, bool&, float&)>& drawSettings);
    void DrawPresetSharpeningStatus(const FrameView& view);
    void DrawHDROutputSettings(const FrameView& view);
    void DrawTextureMemoryPanel(const FrameView& view);
    void DrawAdvancedPanel(float tabCardHeight, const FrameView& view);
    bool BeginSettingsColumns(const char* id, float height, const FrameView& view);
    void NextSettingsColumn(float height);
    void EndSettingsColumns();
    void DrawFrameMeasurements(const FrameView& view, float columnHeight);
    void DrawImageMeasurements(const FrameView& view);
    void DrawStageMeasurements(const FrameView& view, TheosRenderPipeline::Overlay::SettingsPage page);
    void DrawMemoryMeasurements(const FrameView& view);
    void DrawReportingDetails(const FrameView& view);
    void DrawMeasurementControls(const FrameView& view);
    void DrawOutputOptimizations();
    void DrawUIStatusPanel(const FrameView& view);
    void DrawSettingsActions();
	void BuildUI();
    void UpdateUIScale();
    void DrawMenuSizeControl();
    void DrawMenuKeyControl(const FrameView& view);
    void DrawZoomButtons();
	void UpdateFrameStats();
	void HandleHotkey();
	static LRESULT CALLBACK WindowMessage(int code, WPARAM wParam, LPARAM lParam);
	void PollInput();
	void SetTextInputCapture(bool a_capture);
	void SetVisible(bool a_visible);
    void UpdateControlCapture();
    void RefreshNeuralRuntimeAvailability();
	void ApplyNeuralRenderingStateForSession(int a_state);
	void DrawNeuralRenderingPanel(float tabCardHeight, const FrameView& view);
	void DrawFrameGenerationPanel(float tabCardHeight, const FrameView& view);

	bool initialized{ false };
	bool visible{ false };
	bool showDeveloperControls{ false };
    // Presets tab: 0 selects Base; preset IDs are nonzero.
    std::uint32_t presetSelected{}, presetShown{};
    int presetTime{3}, presetWeatherGroup{};
    bool presetTimed{};
    char presetSearch[128]{};
    std::vector<TheosRenderPipeline::Appearance::Record> presetPickerSelection;
    TheosRenderPipeline::Overlay::SettingsPage requestedPage{TheosRenderPipeline::Overlay::SettingsPage::None};
	TheosRenderPipeline::RendererSettingsDraft settingsDraft{};
    bool nrRuntimePresent{false};
    TheosRenderPipeline::Overlay::Layout layout;
    bool layoutPending{true};
    float layoutDisplayWidth{}, layoutDisplayHeight{};
    float menuSizeEdit{100.0f};
    bool menuSizeEditing{false};
	std::string actionMessage;
	bool actionMessageIsError{ false };
    std::string menuKeyError;
    // Capture is cancelled on any frame that does not draw the menu key control.
    bool menuKeyControlDrawn{ false };

	bool controlsSuppressed{ false };
	bool fightingWasEnabled{ true };
	bool lookingWasEnabled{ true };
	bool textInputCaptured{ false };
	TheosRenderPipeline::Overlay::NumericInput numericInput;
	TheosRenderPipeline::Overlay::WindowHotkeys hotkeys;

	IDXGISwapChain* swapChain{ nullptr };
	ID3D11Device* device{ nullptr };
	ID3D11DeviceContext* context{ nullptr };
	HWND hwnd{ nullptr };

	// Presented-frame timing (QPC based).
	long long lastFrameQpc{ 0 };
	double qpcToMs{ 0.0 };
	static constexpr int kFrameHistory = 120;
	float frameTimesMs[kFrameHistory]{};
	int frameTimeIndex{ 0 };
	int frameTimeCount{ 0 };
	uint64_t presentedFrameCount{ 0 };

	// Snapshots for the rendered/presented FPS split (1-second windows).
	long long fpsWindowStartQpc{ 0 };
	uint64_t fpsWindowPresentedStart{ 0 };
	uint64_t fpsWindowRenderedStart{ 0 };
	float presentedFps{ 0.0f };
	float renderedFps{ 0.0f };
	TheosRenderPipeline::Telemetry::OutputRateSampler outputRate;

};
