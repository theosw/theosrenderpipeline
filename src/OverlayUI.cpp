#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "RendererSettingsController.h"
#include "OverlayUIStyle.h"
#include "OverlayRenderTarget.h"

#include <imgui_internal.h>
#include <SimpleIni.h>

#include <PCH.h>

#include "RenderPipeline.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "PerformanceTuning.h"
#include "VideoMemoryTelemetry.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

using namespace TheosRenderPipeline::Overlay;

namespace
{
	void AllowSkyrimTextInput(RE::ControlMap* a_controlMap, bool a_allow)
	{
		// Use the verified engine implementation: textEntryCount is at +0x120
		// on 1.5.97/1.6.640 and +0x128 on 1.6.1170/1.7.104.
		using Func = decltype(&AllowSkyrimTextInput);
		static REL::Relocation<Func> func{ RELOCATION_ID(67252, 68552) };
		func(a_controlMap, a_allow);
	}

}

void OverlayUI::Init(IDXGISwapChain* a_swapChain, ID3D11Device* a_device, ID3D11DeviceContext* a_context)
{
	if (initialized) {
		return;
	}

	swapChain = a_swapChain;
	device = a_device;
	context = a_context;

	DXGI_SWAP_CHAIN_DESC desc{};
	if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow) {
		logger::error("[Overlay] could not get swapchain output window");
		return;
	}
	hwnd = desc.OutputWindow;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;  // no imgui.ini clutter in the game directory
    io.ConfigWindowsResizeFromEdges = true;
    CSimpleIniA menuIni;
    menuIni.SetUnicode();
    if (menuIni.LoadFile(L"Data\\SKSE\\Plugins\\TheosRenderPipeline.ini") >= 0)
        layout = LoadLayout(menuIni);
	ImGui::StyleColorsDark();
	ApplyRendererStyle();
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(device, context);
	VideoMemoryTelemetry::GetSingleton()->Init(device);

	// Both input routes queue hotkeys; only Present changes ImGui or game controls.
	if (const auto error = hotkeys.Install(hwnd,
	        static_cast<UINT>(RenderPipeline::GetSingleton()->mToggleOverlayHotkey), WindowMessage)) {
		util::report_and_fail(std::format("Theo's Render Pipeline: window hotkey observer failed (Win32 {}).", error));
	}
	logger::info("[Overlay Input] window-message observer installed (thread={})", GetWindowThreadProcessId(hwnd, nullptr));

	LARGE_INTEGER freq{};
	::QueryPerformanceFrequency(&freq);
	qpcToMs = 1000.0 / static_cast<double>(freq.QuadPart);

	initialized = true;
	logger::info("[Overlay] initialized (hwnd={}, toggle hotkey vk=0x{:02X})", reinterpret_cast<void*>(hwnd), RenderPipeline::GetSingleton()->mToggleOverlayHotkey);
}

void OverlayUI::PollInput()
{
	// Mouse buttons and numeric editing retain their existing polling.
	ImGuiIO& io = ImGui::GetIO();
	static bool lastLeft = false;
	static bool lastRight = false;
	const auto foreground = ::GetForegroundWindow();
	const bool focused = visible && foreground && (foreground == hwnd || ::IsChild(hwnd, foreground));
	const bool left = focused && (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	const bool right = focused && (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
	if (left != lastLeft) {
		io.AddMouseButtonEvent(0, left);
		lastLeft = left;
	}
	if (right != lastRight) {
		io.AddMouseButtonEvent(1, right);
		lastRight = right;
	}
	TheosRenderPipeline::Overlay::NumericInput::Keys keys{};
	if (focused) {
		for (unsigned vk = VK_BACK; vk < keys.size(); ++vk) {
			keys[vk] = (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
		}
	}
	numericInput.Update(io, keys, focused);
	SetTextInputCapture(focused && io.WantTextInput);
}

void OverlayUI::SetTextInputCapture(bool a_capture)
{
	if (textInputCaptured == a_capture) { return; }
	if (auto* controlMap = RE::ControlMap::GetSingleton()) {
		// Pair only the ownership acquired by this overlay. Other menus may own
		// independent claims on Skyrim's text-input counter.
		AllowSkyrimTextInput(controlMap, a_capture);
		logger::info("[Overlay Input] Skyrim native text capture {}",
			a_capture ? "acquired" : "released");
		textInputCaptured = a_capture;
	}
}

void OverlayUI::SetVisible(bool a_visible)
{
	visible = a_visible;
    if (visible && settingsDraft.valid) {
        RefreshNeuralRuntimeAvailability();
    }
	if (initialized) {
		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = visible;
		if (!visible) {
			numericInput.Update(io, {}, false);
			// Hidden overlays do not run another ImGui frame, so a queued focus-loss
			// event would never clear the active InputInt. Reset it synchronously or
			// the next open can reacquire Skyrim text input before anything is focused.
			ImGui::ClearActiveID();
			ImGui::GetCurrentContext()->WantTextInputNextFrame = 0;
			io.ClearInputKeys();
			io.ClearEventsQueue();
			io.WantTextInput = false;
			SetTextInputCapture(false);
		}
	}

    UpdateControlCapture();
}

void OverlayUI::UpdateControlCapture()
{
    const bool wantsControls = visible || TheosRenderPipeline::ReShadeIntegration::Get().OverlayOpen();
	// Suppress game controls with paired ToggleControls calls and restore the
	// remembered state on close (pattern proven by the SKSE_Template_Forms
	// overlay; a bare ignoreKeyboardMouse write left input permanently dead).
	auto controlMap = RE::ControlMap::GetSingleton();
	if (!controlMap) {
		return;
	}

	if (wantsControls) {
		if (!controlsSuppressed) {
			fightingWasEnabled = controlMap->IsFightingControlsEnabled();
			lookingWasEnabled = controlMap->IsLookingControlsEnabled();
			// Updating stored controls preserves the previous CommonLib wrapper's
			// behavior. The engine owns the version-specific member offsets.
			controlMap->ToggleControls(RE::ControlMap::UEFlag::kFighting, false, true);
			controlMap->ToggleControls(RE::ControlMap::UEFlag::kLooking, false, true);
			controlsSuppressed = true;
		}
	} else if (controlsSuppressed) {
		controlMap->ToggleControls(RE::ControlMap::UEFlag::kFighting, fightingWasEnabled, true);
		controlMap->ToggleControls(RE::ControlMap::UEFlag::kLooking, lookingWasEnabled, true);
		controlsSuppressed = false;
	}
}

void OverlayUI::ApplyNeuralRenderingStateForSession(int state)
{
    auto result = TheosRenderPipeline::RendererSettingsController::Current().SetNeuralRenderingEnabled(state != 0);
    logger::info("[Overlay] NR shortcut requested={} applied={} error={} detail={}",
        state != 0, result.applied, result.error, result.message);
    actionMessage = std::move(result.message);
    actionMessageIsError = result.error;
    if (result.applied)
    {
        settingsDraft.sourceDLSSG.neuralEnabled = state != 0;
    }
}

LRESULT CALLBACK OverlayUI::WindowMessage(int code, WPARAM wParam, LPARAM lParam)
{
	return GetSingleton()->hotkeys.ForwardMessage(code, wParam, lParam);
}

void OverlayUI::HandleHotkey()
{
	const auto toggleKey = static_cast<UINT>(RenderPipeline::GetSingleton()->mToggleOverlayHotkey);
	for (const auto key : hotkeys.TakePending()) {
		const auto actions = ActionsForHotkey(key, toggleKey, visible && ImGui::GetIO().WantTextInput,
            RenderPipeline::GetSingleton()->mEnableNRHotkeys);
		if (actions.neuralState >= 0) { ApplyNeuralRenderingStateForSession(actions.neuralState); }
		if (actions.toggle) {
			SetVisible(!visible);
			logger::info("[Overlay] hotkey vk=0x{:02X} {}", key, visible ? "opened" : "closed");
		}
	}
}

void OverlayUI::UpdateFrameStats()
{
	// Present stops while the game is loading, paused behind another window, or
	// held for a screenshot. Those wall-clock gaps are not rendered frame time
	// and must not contaminate the rolling graph or one-second FPS windows.
	static constexpr double kFrameTimelineDiscontinuityMs = 250.0;

	LARGE_INTEGER now{};
	::QueryPerformanceCounter(&now);
	++presentedFrameCount;
	const auto upscaler = RenderPipeline::GetSingleton();
	bool timelineDiscontinuity = false;

	if (lastFrameQpc != 0) {
		const auto dtMs = static_cast<float>((now.QuadPart - lastFrameQpc) * qpcToMs);
		if (dtMs > 0.0f && dtMs <= kFrameTimelineDiscontinuityMs) {
			frameTimesMs[frameTimeIndex] = dtMs;
			frameTimeIndex = (frameTimeIndex + 1) % kFrameHistory;
			frameTimeCount = frameTimeCount < kFrameHistory ? frameTimeCount + 1 : kFrameHistory;
			PerformanceTuning::GetSingleton()->RecordGameFrameCadenceMs(dtMs);
		} else {
			timelineDiscontinuity = true;
		}
	}
	lastFrameQpc = now.QuadPart;

	// Refresh the rendered/presented FPS split once per second.
	if (fpsWindowStartQpc == 0 || timelineDiscontinuity) {
		presentedFps = renderedFps = 0.0f;
		fpsWindowStartQpc = now.QuadPart;
		fpsWindowPresentedStart = presentedFrameCount;
		fpsWindowRenderedStart = upscaler->mRenderedFrameCount;
	} else {
		const auto windowMs = (now.QuadPart - fpsWindowStartQpc) * qpcToMs;
		if (windowMs >= 1000.0) {
			presentedFps = static_cast<float>((presentedFrameCount - fpsWindowPresentedStart) * 1000.0 / windowMs);
			renderedFps = static_cast<float>((upscaler->mRenderedFrameCount - fpsWindowRenderedStart) * 1000.0 / windowMs);
			fpsWindowStartQpc = now.QuadPart;
			fpsWindowPresentedStart = presentedFrameCount;
			fpsWindowRenderedStart = upscaler->mRenderedFrameCount;
		}
	}

	// DLSS-G output is downstream of the game-facing Present. Read the source
	// session's accumulated runtime deltas. Do not call slDLSSGGetState
	// again from the menu because it consumes the delta.
	TheosRenderPipeline::Telemetry::OutputCounter output{};

		if (NvidiaHost::GetSingleton()->StartupConfigured()) {
			const auto& source = TheosRenderPipeline::SourceDLSSG::Backend::Get();
			const auto& session = source.Snapshot();
			output = { TheosRenderPipeline::Telemetry::OutputSource::Streamline,
				session.presentationEpoch, session.stateQueries, session.runtimePresentedFrames,
				source.Ready() && session.stateQueries > 0 &&
					session.stage != TheosRenderPipeline::SourceDLSSG::SessionStage::Stopped &&
					session.stage != TheosRenderPipeline::SourceDLSSG::SessionStage::Faulted &&
					session.state.status == sl::DLSSGStatus::eOk };
		}

	outputRate.Update(now.QuadPart * qpcToMs, output, timelineDiscontinuity);
}

void OverlayUI::RefreshNeuralRuntimeAvailability()
{
#if !defined(TRP_NO_NEURAL_RENDERING)
    // Refresh on menu open/settings actions, not on every rendered frame.
    nrRuntimePresent = TheosRenderPipeline::SourceDLSSG::NeuralRuntimePresent(
        SourceFrameGeneration::GetSingleton()->settings.neuralRenderingRuntimePath);
    settingsDraft.sourceDLSSG.neuralEnabled &= nrRuntimePresent;
#endif
}

void OverlayUI::CaptureSettingsDraft()
{
    RefreshNeuralRuntimeAvailability();
    settingsDraft = TheosRenderPipeline::RendererSettingsController::Current().Capture(nrRuntimePresent);
}

int OverlayUI::CountStagedChanges() const
{
    return TheosRenderPipeline::RendererSettingsController::Current().CountChanges(settingsDraft, nrRuntimePresent);
}

void OverlayUI::ApplySettingsDraft(bool save)
{
    if (!settingsDraft.valid)
    {
        return;
    }
    auto result = TheosRenderPipeline::RendererSettingsController::Current().Apply(
        settingsDraft, save, save ? &layout : nullptr);
    actionMessage = std::move(result.message);
    actionMessageIsError = result.error;
    if (result.applied)
    {
        CaptureSettingsDraft();
    }
}

void OverlayUI::BuildUI()
{
    const auto view = CaptureFrameView();
    const auto displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 0 || displaySize.y <= 0)
        return;
    if (layoutPending || layoutDisplayWidth != displaySize.x || layoutDisplayHeight != displaySize.y)
    {
        layout = FitLayout(layout, displaySize.x, displaySize.y);
        ImGui::SetNextWindowSize(ImVec2(layout.width, layout.height), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(layout.x, layout.y), ImGuiCond_Always);
        layoutDisplayWidth = displaySize.x;
        layoutDisplayHeight = displaySize.y;
        layoutPending = false;
    }
    ImGui::SetNextWindowSizeConstraints(
        ImVec2((std::min)(780.0f, displaySize.x), (std::min)(560.0f, displaySize.y)), displaySize);
    if (!ImGui::Begin(Plugin::DISPLAY_NAME.data(), nullptr, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    const auto windowPos = ImGui::GetWindowPos();
    const auto windowSize = ImGui::GetWindowSize();
    layout.x = windowPos.x;
    layout.y = windowPos.y;
    layout.width = windowSize.x;
    layout.height = windowSize.y;
    DrawPipelineSummary(view);
    const auto& layoutStyle = ImGui::GetStyle();
    float reservedActionHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeight() +
                                       ImGui::GetTextLineHeightWithSpacing() +
                                       layoutStyle.CellPadding.y * 2.0f + layoutStyle.ItemSpacing.y * 2.0f + 1.0f;
    if (actionMessageIsError && !actionMessage.empty()) {
        const float statusWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - 450.0f - layoutStyle.CellPadding.x * 4.0f);
        reservedActionHeight += (std::max)(0.0f, ImGui::CalcTextSize(actionMessage.c_str(), nullptr, false, statusWidth).y - ImGui::GetFrameHeight());
    }
    const float tabCardHeight = (std::max)(220.0f, ImGui::GetContentRegionAvail().y - reservedActionHeight);

    if (ImGui::BeginTabBar("##theosrenderpipelineTabs", ImGuiTabBarFlags_None))
    {
        DrawImagePanel(tabCardHeight, view);

#if !defined(TRP_NO_NEURAL_RENDERING)
        DrawNeuralRenderingPanel(tabCardHeight, view);
#endif

        DrawFrameGenerationPanel(tabCardHeight, view);
        DrawAdvancedPanel(tabCardHeight, view);
        ImGui::EndTabBar();
    }

    requestedPage = SettingsPage::None;
    DrawSettingsActions();

    ImGui::End();
}

void OverlayUI::OnPresent(ID3D11Texture2D* producerUI)
{
	if (!initialized) {
		return;
	}

	UpdateFrameStats();
    auto* host = NvidiaHost::GetSingleton();
    if (host->ProxyActive() && host->UpscalerReady()) {
        ID3D11Texture2D* target = producerUI;
        if (!target && host->NativePresentReady()) {
            target = host->NativeUIDrawnThisFrame() ? host->NativeUIRenderTexture() : host->NativePresentationTexture();
        }
        if (target) {
            auto& effects = TheosRenderPipeline::ReShadeIntegration::Get();
            const auto result = effects.FinishUI(target);
            if (FAILED(result) && (effects.Snapshot().failures <= 3 || host->PresentCount() % 600 == 0)) {
                logger::warn("[ReShade] {} (0x{:08X})", effects.Status(), static_cast<unsigned>(result));
            }
        }
    }
	HandleHotkey();
    UpdateControlCapture();

	if (!visible) {
		return;
	}

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	PollInput();
	auto* nvidiaHost = NvidiaHost::GetSingleton();
	if (nvidiaHost->ProxyActive()) {
		// Source Present preparation also provides a native target on frames
		// without a world/Mist UI handoff. Legacy fallback keeps its old extent.
		const bool nativeUI = producerUI || ((nvidiaHost->NativePresentReady() || nvidiaHost->NativeUIPassActive()) && nvidiaHost->NativePresentationTexture());
		ImGui::GetIO().DisplaySize = ImVec2(
			static_cast<float>(nativeUI ? nvidiaHost->OutputWidth() : nvidiaHost->RenderWidth()),
			static_cast<float>(nativeUI ? nvidiaHost->OutputHeight() : nvidiaHost->RenderHeight()));
	}
	ImGui::NewFrame();

	BuildUI();

	ImGui::Render();
	ID3D11RenderTargetView* overlayTarget = nullptr;
	ID3D11Texture2D* finalFrame = nullptr;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> fallbackBuffer;
	if (producerUI) {
		finalFrame = producerUI;
	} else if (nvidiaHost->ProxyActive()) {
		finalFrame = nvidiaHost->NativeUIPassActive() ?
			nvidiaHost->NativeUIRenderTexture() : nvidiaHost->GameFacingTexture();
		if (nvidiaHost->NativePresentReady()) {
			finalFrame = nvidiaHost->NativeUIDrawnThisFrame() ? nvidiaHost->NativeUIRenderTexture() : nvidiaHost->NativePresentationTexture();
		}
		if (!finalFrame) {
			static std::atomic_bool loggedUnavailable{ false };
			if (!loggedUnavailable.exchange(true)) {
				logger::error("[Overlay] NVIDIA host render target is unavailable");
			}
			return;
		}
		if (nvidiaHost->NativePresentReady()) {
			// Borrow the host's view. Retaining an inner swapchain buffer here
			// would prevent resize when this overlay is subsequently hidden.
			overlayTarget = nvidiaHost->NativeUIDrawnThisFrame() ? nvidiaHost->NativeUIRenderRTV() : nvidiaHost->NativePresentationRTV();
		}
	} else {
		const auto result = swapChain->GetBuffer(0, IID_PPV_ARGS(&fallbackBuffer));
		if (FAILED(result)) {
			logger::error("[Overlay] backbuffer unavailable hr=0x{:08X}", static_cast<std::uint32_t>(result));
			return;
		}
		finalFrame = fallbackBuffer.Get();
	}
	const auto result = DrawWithRenderTarget(device, finalFrame, overlayTarget, [&](ID3D11RenderTargetView* target) {
		context->OMSetRenderTargets(1, &target, nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	});
	if (FAILED(result)) {
		logger::error("[Overlay] render target view creation failed hr=0x{:08X}", static_cast<std::uint32_t>(result));
	}
}

void OverlayUI::DrawSettingsActions()
{
    ImGui::Separator();
    const int stagedChanges = CountStagedChanges();
    if (ImGui::BeginTable("##actionBar", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("##actionStatus", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 450.0f);
        ImGui::TableNextColumn();
        const auto status = TheosRenderPipeline::SettingsStatus(stagedChanges, actionMessage, actionMessageIsError);
        using StatusKind = TheosRenderPipeline::SettingsStatusKind;
        ImGui::PushTextWrapPos(0);
        if (status.kind == StatusKind::Neutral) { ImGui::TextDisabled("%s", status.text.c_str()); }
        else { ImGui::TextColored(status.kind == StatusKind::Error ? kRust :
            status.kind == StatusKind::Pending ? kAmber : kSage, "%s", status.text.c_str()); }
        ImGui::PopTextWrapPos();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(stagedChanges == 0);
        if (ImGui::Button("Discard", ImVec2(110.0f, 0.0f)))
        {
            CaptureSettingsDraft();
            actionMessage = "Unapplied edits discarded.";
            actionMessageIsError = false;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                "Discard edits you have not applied. Applied settings and saved defaults stay as they are.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply", ImVec2(100.0f, 0.0f)))
        {
            ApplySettingsDraft(false);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Apply live settings for this session. Saved defaults stay unchanged.\nMode and render "
                              "scale changes require Save and restart.");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.07f, 0.04f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, kAmber);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOchre);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAmberDim);
        if (ImGui::Button("Save as default", ImVec2(200.0f, 0.0f)))
        {
            ApplySettingsDraft(true);
        }
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Apply live settings and save your choices, window layout and divider for future launches.\n"
                              "Mode and render scale changes take effect after restarting.");
        }
        ImGui::EndTable();
    }
}
