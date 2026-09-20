#pragma once

#include "OverlayHotkeys.h"

namespace TheosRenderPipeline::Overlay
{
    template <class Event, class Device>
    std::vector<UINT> CollectGameHotkeyPresses(Event* event, Device keyboard)
    {
        std::vector<UINT> pressed;
        for (; event; event = event->next) {
            const auto* button = event->AsButtonEvent();
            if (!button || button->GetDevice() != keyboard || !button->IsDown()) { continue; }
            if (const auto key = VirtualKeyFromGameScanCode(button->GetIDCode())) { pressed.push_back(key); }
        }
        return pressed;
    }

    // Register once when Skyrim's input devices are available. Does not patch
    // the input dispatcher or consume events belonging to other menus.
    void RegisterGameInput();
}
