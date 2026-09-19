#pragma once

#include <Windows.h>
#include <mutex>
#include <string>
#include <vector>

namespace TheosRenderPipeline::Overlay
{
    // The picker accepts single keyboard keys. Keep existing INI-only mouse and
    // modifier bindings readable; Escape cancels capture and brackets control NR.
    inline bool IsBindableKeyboardKey(UINT key)
    {
        if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
            (key >= VK_NUMPAD0 && key <= VK_DIVIDE) || (key >= VK_F1 && key <= VK_F24)) {
            return true;
        }
        switch (key) {
        case VK_BACK: case VK_TAB: case VK_RETURN: case VK_PAUSE: case VK_CAPITAL:
        case VK_SPACE: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_SNAPSHOT:
        case VK_INSERT: case VK_DELETE: case VK_NUMLOCK: case VK_SCROLL:
        case VK_OEM_1: case VK_OEM_PLUS: case VK_OEM_COMMA: case VK_OEM_MINUS:
        case VK_OEM_PERIOD: case VK_OEM_2: case VK_OEM_3: case VK_OEM_5:
        case VK_OEM_7: case VK_OEM_8: case VK_OEM_102:
            return true;
        default: return false;
        }
    }

    inline bool CanToggleWhileEditing(UINT key)
    {
        return key == VK_END || (key >= VK_F1 && key <= VK_F24);
    }

    std::string HotkeyName(UINT key);

    template <class Ini>
    int LoadMenuHotkey(const Ini& ini)
    {
        const auto key = ini.GetLongValue("Hotkeys", "ToggleOverlay", VK_END);
        return key > 0 && key <= 0xFE ? static_cast<int>(key) : VK_END;
    }

    template <class Ini>
    void StoreMenuHotkey(Ini& ini, int key)
    {
        ini.SetLongValue("Hotkeys", "ToggleOverlay", key, nullptr, true);
    }

    enum class CaptureStatus { Idle, Waiting, Accepted, Cancelled, Rejected };
    struct HotkeyCapture
    {
        CaptureStatus status{CaptureStatus::Idle};
        UINT key{};
    };

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
        // End and function keys close active text fields; typing/navigation keys
        // retain their editing behavior until the field loses focus.
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
        std::vector<UINT> TakePending();
        void SetToggleKey(UINT key);
        void BeginCapture();
        void CancelCapture();
        bool IsCapturing();
        HotkeyCapture TakeCapture();

    private:
        void ObserveMessage(const MSG& message);
        bool HasFocus() const;
        std::mutex mutex_;
        HWND window_{};
        HHOOK hook_{};
        UINT toggleKey_{};
        ForegroundQuery foreground_{ ::GetForegroundWindow };
        std::vector<UINT> pending_;
        HotkeyCapture capture_;
    };
}
