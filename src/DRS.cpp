#include "DRS.h"

#include "RenderPipeline.h"
#include "FrameTrace.h"

#include <string>
#include <unordered_set>

void DRS::GetGameSettings()
{
	bEnableAutoDynamicResolution = RE::GetINISetting("bEnableAutoDynamicResolution:Display");
	if (bEnableAutoDynamicResolution) {
		if (!bEnableAutoDynamicResolution->GetBool()) {
			logger::info("Enabling Skyrim resolution-state updates; scaled-proxy mode pins the engine ratio to 1.0");
		}
		bEnableAutoDynamicResolution->data.b = true;
	} else {
		logger::warn("Unable to enable Skyrim resolution-state updates; fixed-ratio guard may not run");
	}
}

void DRS::SetDRS(BSGraphics::State* a_state)
{
	auto& runtimeData = a_state->GetRuntimeData();
	// The host supplies a render-sized buffer. Another engine-side scale
	// would reduce it twice and leave downstream attachments mismatched.
	// Pin both current and previous fields through CommonLib's verified layout.
	runtimeData.dynamicResolutionPreviousWidthRatio = 1.0f;
	runtimeData.dynamicResolutionPreviousHeightRatio = 1.0f;
	runtimeData.dynamicResolutionWidthRatio = 1.0f;
	runtimeData.dynamicResolutionHeightRatio = 1.0f;
}

void DRS::MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		GetGameSettings();
		break;
	}
}

RE::BSEventNotifyControl MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	static bool mainOpen = false;
	static bool loadingOpen = false;
	static bool raceOpen = false;
	static bool faderOpen = false;
	bool transitionEvent = true;
	auto boundaryReason = FrameTrace::BoundaryReason::kMainMenu;
	if (a_event->menuName == RE::MainMenu::MENU_NAME) {
		mainOpen = a_event->opening;
	} else if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
		loadingOpen = a_event->opening;
		boundaryReason = FrameTrace::BoundaryReason::kLoadingMenu;
	} else if (a_event->menuName == RE::RaceSexMenu::MENU_NAME) {
		raceOpen = a_event->opening;
		boundaryReason = FrameTrace::BoundaryReason::kRaceMenu;
	} else if (a_event->menuName == RE::FaderMenu::MENU_NAME) {
		faderOpen = a_event->opening;
		boundaryReason = FrameTrace::BoundaryReason::kFaderMenu;
	} else {
		transitionEvent = false;
	}

	if (transitionEvent) {
		FrameTrace::GetSingleton()->Record(
			FrameTrace::EventType::kBoundary,
			a_event->opening ? FrameTrace::kBoundaryBegin : FrameTrace::kBoundaryEnd,
			0,
			0,
			static_cast<std::uint64_t>(boundaryReason));
		RenderPipeline::GetSingleton()->SetFrameGenerationTransitionBlocked(
			mainOpen || loadingOpen || raceOpen || faderOpen);
	}

	if (std::strcmp(a_event->menuName.c_str(), "Console") == 0) {
		auto upscaler = RenderPipeline::GetSingleton();
		upscaler->mConsoleOpen.store(a_event->opening, std::memory_order_release);
		upscaler->mConsoleDiagnosticsActive.store(
			a_event->opening && upscaler->mLogMenuMetrics,
			std::memory_order_release);
		if (a_event->opening) {
			const auto generation = upscaler->mConsoleDiagnosticGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
			logger::info("[ConsoleDiag] session {} opened", generation);
		} else {
			logger::info("[ConsoleDiag] session closed");
		}
	}

	if (a_event->menuName == RE::MainMenu::MENU_NAME ||
		a_event->menuName == RE::LoadingMenu::MENU_NAME ||
		a_event->menuName == RE::RaceSexMenu::MENU_NAME ||
		a_event->menuName == RE::FaderMenu::MENU_NAME) {
		// Stable markers for the automated test harness.
		logger::info("[Menu] {} {}", a_event->menuName.c_str(), a_event->opening ? "opened" : "closed");
	}

	// Ultrawide investigation: how does the engine place each menu movie on
	// the screen? Stage size vs viewport rect vs scale mode tells us, per
	// menu, which mechanism (letterbox, stretch, safe zone) is in charge.
	if (a_event->opening && RenderPipeline::GetSingleton()->mLogMenuMetrics) {
		if (auto ui = RE::UI::GetSingleton()) {
			if (auto menu = ui->GetMenu(a_event->menuName)) {
				if (auto& movie = menu->uiMovie) {
					RE::GViewport viewport{};
					movie->GetViewport(&viewport);
					const auto def = movie->GetMovieDef();
					const float stageW = def ? def->GetWidth() : 0.0f;
					const float stageH = def ? def->GetHeight() : 0.0f;
					const auto safeX = RE::GetINISetting("fSafeZoneXWide:Interface");
					const auto safeY = RE::GetINISetting("fSafeZoneYWide:Interface");
					logger::info(
						"[MenuMetrics] \"{}\" stage {:.0f}x{:.0f} viewport ({},{} {}x{}) buffer {}x{} scale {:.4f} flags 0x{:X} depth {} safeZoneWide ({:.0f},{:.0f})",
						a_event->menuName.c_str(),
						stageW, stageH,
						viewport.left, viewport.top, viewport.width, viewport.height,
						viewport.bufferWidth, viewport.bufferHeight,
						viewport.scale,
						viewport.flags.underlying(),
						static_cast<int>(menu->depthPriority),
						safeX ? safeX->GetFloat() : -1.0f,
						safeY ? safeY->GetFloat() : -1.0f);
				} else {
					logger::info("[MenuMetrics] \"{}\" (no movie)", a_event->menuName.c_str());
				}
			}
		}
	}

	if (a_event->menuName == RE::MainMenu::MENU_NAME ||
		a_event->menuName == RE::LoadingMenu::MENU_NAME ||
		a_event->menuName == RE::RaceSexMenu::MENU_NAME) {
		// Track which reset-menus are open: close events arrive after the next
		// menu's open event (Loading closes after Main opens), so last-event-
		// wins would resume temporal jitter while the main menu was still up.
		const bool anyOpen = mainOpen || loadingOpen || raceOpen;
		if (a_event->opening) {
			DRS::GetSingleton()->reset = true;
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
			if (a_event->menuName == RE::MainMenu::MENU_NAME) {
				RenderPipeline::GetSingleton()->ArmAutoLoad();
			}
#endif
		} else if (!anyOpen && a_event->menuName != RE::MainMenu::MENU_NAME) {
			// Main-menu close always precedes a load (or quit): stay reset
			// through the gap until the loading menu closes at world entry.
			DRS::GetSingleton()->reset = false;
			// Flush DLSS temporal history accumulated across the transition.
			RenderPipeline::GetSingleton()->RequestHistoryReset();
		}
	} else if (a_event->menuName == RE::FaderMenu::MENU_NAME) {
		if (!a_event->opening && !DRS::GetSingleton()->reset) {
			RenderPipeline::GetSingleton()->RequestHistoryReset();
		}
	}

	return RE::BSEventNotifyControl::kContinue;
}

bool MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto                             ui = RE::UI::GetSingleton();

	if (!ui) {
		logger::error("UI event source not found");
		return false;
	}

	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

	logger::info("Registered menu open/close handler");

	return true;
}
