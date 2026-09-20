#pragma once

#include <Windows.h>
#include <array>
#include <mutex>
#include <vector>

namespace TheosRenderPipeline::Overlay
{
    struct HotkeyActions
    {
        int neuralState{ -1 };
        bool toggle{};
    };

    inline HotkeyActions ActionsForHotkey(UINT key, UINT toggleKey, bool editing)
    {
        HotkeyActions actions;
#if !defined(TRP_NO_NEURAL_RENDERING)
        if (!editing && key == VK_OEM_4) { actions.neuralState = 0; }
        if (!editing && key == VK_OEM_6) { actions.neuralState = 1; }
#endif
        // End can close an active text field; a numeric binding must not toggle
        // the menu while that number is being typed.
        actions.toggle = key == toggleKey && (!editing || toggleKey == VK_END);
        return actions;
    }

    class WindowHotkeys
    {
    public:
        using ForegroundQuery = HWND (WINAPI*)();

        ~WindowHotkeys();
        // Install once for this process's window thread. The callback delegates
        // to ForwardMessage; only copied hotkey values cross to Present.
        DWORD Install(HWND window, UINT toggleKey, HOOKPROC callback,
                      ForegroundQuery foreground = ::GetForegroundWindow);
        void Uninstall();
        LRESULT ForwardMessage(int code, WPARAM wParam, LPARAM lParam);
        // One complete Skyrim input batch, including an empty batch. Values are
        // copied; no InputEvent pointer or ImGui state crosses threads.
        void ObserveGameKeys(const std::vector<UINT>& pressedKeys);
        std::vector<UINT> TakePending();

    private:
        void ObserveMessage(const MSG& message);
        UINT NormalizeKey(UINT key) const;
        bool IsHotkey(UINT key) const;
        void ClearPending();
        bool HasFocus() const;
        std::mutex mutex_;
        HWND window_{};
        HHOOK hook_{};
        UINT toggleKey_{};
        ForegroundQuery foreground_{ ::GetForegroundWindow };
        std::vector<UINT> pending_;
        // Match copies of a press across adjacent window/game input batches,
        // independently of when Present drains the actions. Credits expire at
        // the next game batch so a consuming menu cannot leave stale presses.
        std::array<unsigned, 256> windowPresses_{};
        std::array<unsigned, 256> gamePresses_{};
    };

    // Skyrim keyboard IDs are DirectInput scan codes, not Win32 virtual keys.
    UINT VirtualKeyFromGameScanCode(UINT scanCode);
}
