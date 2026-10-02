#pragma once

#include <Windows.h>
#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace TheosRenderPipeline::Overlay
{
    // Single keyboard keys the in-game picker can assign. Escape cancels the
    // picker and modifiers are skipped, so Shift+F10 still assigns F10.
    // Existing INI-only mouse and modifier bindings keep working.
    inline bool IsBindableKeyboardKey(UINT key)
    {
        if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
            (key >= VK_NUMPAD0 && key <= VK_DIVIDE) || (key >= VK_F1 && key <= VK_F24) ||
            (key >= VK_OEM_1 && key <= VK_OEM_3) || (key >= VK_OEM_4 && key <= VK_OEM_8)) {
            return true;
        }
        switch (key) {
        case VK_BACK: case VK_TAB: case VK_RETURN: case VK_PAUSE: case VK_SPACE:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_LEFT: case VK_UP:
        case VK_RIGHT: case VK_DOWN: case VK_INSERT: case VK_DELETE: case VK_NUMLOCK:
        case VK_SCROLL: case VK_OEM_102:
            return true;
        default: return false;
        }
    }

    inline bool IsModifierKey(UINT key)
    {
        return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
            (key >= VK_LSHIFT && key <= VK_RMENU);
    }

    // Keys that neither type nor edit a text field can close the menu while a
    // field is active. Other bindings keep editing until the field loses focus.
    inline bool CanToggleWhileEditing(UINT key)
    {
        return key == VK_END || (key >= VK_F1 && key <= VK_F24) || key == VK_INSERT ||
            key == VK_PRIOR || key == VK_NEXT || key == VK_PAUSE || key == VK_SCROLL;
    }

    inline bool ReservedForNRHotkeys([[maybe_unused]] UINT key, [[maybe_unused]] bool enableNRHotkeys)
    {
#if !defined(TRP_NO_NEURAL_RENDERING)
        return enableNRHotkeys && (key == VK_OEM_4 || key == VK_OEM_6);
#else
        return false;
#endif
    }

    // Display name for the current keyboard layout, with explicit navigation
    // extended flags so End is not shown as Num 1.
    std::string HotkeyName(UINT key);

    template<class Ini>
    int LoadMenuHotkey(const Ini& ini)
    {
        const auto key = ini.GetLongValue("Hotkeys", "ToggleOverlay", VK_END);
        return key > 0 && key <= 0xFE ? static_cast<int>(key) : VK_END;
    }

    template<class Ini>
    void StoreMenuHotkey(Ini& ini, int key)
    {
        ini.SetLongValue("Hotkeys", "ToggleOverlay", key, nullptr, true);
    }

    enum class CaptureStatus { Idle, Waiting, Accepted, Cancelled, Rejected };
    struct HotkeyCapture
    {
        CaptureStatus status{ CaptureStatus::Idle };
        UINT key{};
    };

    struct HotkeyActions
    {
        int neuralState{ -1 };
        bool toggle{};
    };

    template<class Ini>
    bool LoadNRHotkeysEnabled(const Ini& ini)
    {
        return ini.GetBoolValue("Hotkeys", "EnableNRHotkeys", false);
    }

    inline HotkeyActions ActionsForHotkey(UINT key, UINT toggleKey, bool editing, bool enableNRHotkeys)
    {
        HotkeyActions actions;
#if !defined(TRP_NO_NEURAL_RENDERING)
        if (enableNRHotkeys && !editing && key == VK_OEM_4) { actions.neuralState = 0; }
        if (enableNRHotkeys && !editing && key == VK_OEM_6) { actions.neuralState = 1; }
#endif
        // A binding that types, such as a digit, must not toggle the menu while
        // that character is being typed.
        actions.toggle = key == toggleKey && (!editing || CanToggleWhileEditing(toggleKey));
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
        void SetToggleKey(UINT key);
        // While capturing, the next key-down from either route becomes the
        // result and no hotkey actions are queued until Present takes it.
        void BeginCapture();
        void CancelCapture();
        bool IsCapturing();
        HotkeyCapture TakeCapture();

    private:
        void ObserveMessage(const MSG& message);
        UINT NormalizeKey(UINT key) const;
        bool IsHotkey(UINT key) const;
        void ClearPending();
        void LoseFocus();
        void CaptureKey(UINT key, std::array<unsigned, 256>& route, std::array<unsigned, 256>& other);
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
        HotkeyCapture capture_;
    };

    // Skyrim keyboard IDs are DirectInput scan codes, not Win32 virtual keys.
    UINT VirtualKeyFromGameScanCode(UINT scanCode);
}
