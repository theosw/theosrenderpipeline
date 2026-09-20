#include <PCH.h>
#include "OverlayGameInput.h"
#include "OverlayUI.h"

namespace TheosRenderPipeline::Overlay
{
    namespace
    {
        class GameInput final : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events,
                RE::BSTEventSource<RE::InputEvent*>*) override
            {
                const auto pressed = CollectGameHotkeyPresses(events ? *events : nullptr, RE::INPUT_DEVICE::kKeyboard);
                // Even an empty batch closes the previous matching interval.
                OverlayUI::GetSingleton()->ObserveGameHotkeys(pressed);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void RegisterGameInput()
    {
        static GameInput sink;
        static bool registered = false;
        if (registered) { return; }
        if (auto* manager = RE::BSInputDeviceManager::GetSingleton()) {
            manager->AddEventSink(&sink);
            registered = true;
            logger::info("[Overlay Input] Skyrim keyboard event sink registered");
        } else {
            logger::warn("[Overlay Input] Skyrim input manager unavailable; window hotkeys remain active");
        }
    }
}
