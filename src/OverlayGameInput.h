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

    // Mouse wheel notches in one batch: +1 per wheel-up press, -1 per wheel-down.
    // Skyrim reads the mouse through DirectInput, so Windows may never post
    // WM_MOUSEWHEEL; its own input events always carry the wheel.
    template <class Event, class Device>
    int CollectGameWheel(Event* event, Device mouse, std::uint32_t up, std::uint32_t down)
    {
        int notches = 0;
        for (; event; event = event->next) {
            const auto* button = event->AsButtonEvent();
            if (!button || button->GetDevice() != mouse || !button->IsDown()) { continue; }
            if (button->GetIDCode() == up) { ++notches; } else if (button->GetIDCode() == down) { --notches; }
        }
        return notches;
    }

    // Register once when Skyrim's input devices are available. Does not patch
    // the input dispatcher or consume events belonging to other menus.
    void RegisterGameInput();
}
