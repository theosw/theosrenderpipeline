#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "TextureProviderBridge.h"
#include "FrameGen/SourceDLSSGSettings.h"
#include "FrameTelemetry.h"
#include "OverlayNumericInput.h"
#include "OverlayHotkeys.h"
#include "RendererSettings.h"
#include "OverlayPipeline.h"

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

private:
	OverlayUI() = default;
	OverlayUI(const OverlayUI&) = delete;
	OverlayUI& operator=(const OverlayUI&) = delete;

    struct FrameView;
    FrameView CaptureFrameView();
    void DrawPipelineSummary(const FrameView& view);
    void DrawImagePanel(float tabCardHeight, float nestedCardHeight, const FrameView& view);
    void DrawTextureMemoryPanel(float nestedCardHeight, const FrameView& view);
    void DrawAdvancedPanel(float advancedCardHeight, const FrameView& view);
    void DrawUIStatusPanel(float advancedCardHeight);
    void DrawRuntimePanel(float advancedCardHeight, const FrameView& view);
    void DrawSettingsActions();
	void BuildUI();
	void UpdateFrameStats();
	void HandleHotkey();
	static LRESULT CALLBACK WindowMessage(int code, WPARAM wParam, LPARAM lParam);
	void PollInput();
	void SetTextInputCapture(bool a_capture);
	void SetVisible(bool a_visible);
    void UpdateControlCapture();
	void CaptureSettingsDraft();
    void RefreshNeuralRuntimeAvailability();
	int CountStagedChanges() const;
	void ApplySettingsDraft(bool a_saveAsDefault);
	void ApplyNeuralRenderingStateForSession(int a_state);
	void DrawNeuralRenderingPanel(float tabCardHeight);
	void DrawPerformancePanel(float advancedCardHeight, const TheosRenderPipeline::SourceDLSSG::NeuralSnapshot& sourceNeural);

	struct FrameGenerationView
	{
		bool sourceDLSSGActive;
		bool frameGenerationRuntimeActive;
		unsigned activeDisplayMultiplier;
		const char* outputLabel;
		const std::string& outputText;
		const TheosRenderPipeline::SourceDLSSG::NeuralSnapshot& sourceNeural;
	};
	void DrawFrameGenerationPanel(float tabCardHeight, float nestedCardHeight, const FrameGenerationView& view);

	bool initialized{ false };
	bool visible{ false };
	bool showDeveloperControls{ false };
    TheosRenderPipeline::Overlay::SettingsPage requestedPage{TheosRenderPipeline::Overlay::SettingsPage::None};
	TheosRenderPipeline::RendererSettingsDraft settingsDraft{};
    bool nrRuntimePresent{false};
	std::string actionMessage;
    std::string hotkeyCaptureError;
	bool actionMessageIsError{ false };

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
