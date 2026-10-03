#include "OverlayHotkeys.h"

#include <cstdio>

namespace TheosRenderPipeline::Overlay
{
    std::string HotkeyName(UINT key)
    {
        if (key >= VK_F1 && key <= VK_F24) { return "F" + std::to_string(key - VK_F1 + 1); }
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) { return "Num " + std::to_string(key - VK_NUMPAD0); }
        switch (key) {
        case VK_LBUTTON: return "Mouse 1";
        case VK_RBUTTON: return "Mouse 2";
        case VK_MBUTTON: return "Mouse 3";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        default: break;
        }
        const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
        LONG flags = static_cast<LONG>((scan & 0xFF) << 16);
        // VK-to-scan mapping does not retain E0 for navigation keys on every
        // layout. Keep them distinct from the numeric keypad.
        if ((scan & 0xFF00) != 0 || key == VK_INSERT || key == VK_DELETE || key == VK_HOME ||
            key == VK_END || key == VK_PRIOR || key == VK_NEXT || key == VK_LEFT || key == VK_RIGHT ||
            key == VK_UP || key == VK_DOWN || key == VK_DIVIDE || key == VK_NUMLOCK ||
            key == VK_RCONTROL || key == VK_RMENU) {
            flags |= 1 << 24;
        }
        wchar_t name[128]{};
        char utf8[512]{};
        if (scan && GetKeyNameTextW(flags, name, 128) > 0 &&
            WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, 512, nullptr, nullptr) > 0) {
            return utf8;
        }
        char hex[16]{};
        std::snprintf(hex, sizeof(hex), "Key 0x%02X", key);
        return hex;
    }

    WindowHotkeys::~WindowHotkeys() { Uninstall(); }

    DWORD WindowHotkeys::Install(HWND window, UINT toggleKey, HOOKPROC callback, ForegroundQuery foreground)
    {
        if (hook_) { return ERROR_ALREADY_EXISTS; }
        DWORD process{};
        const auto thread = GetWindowThreadProcessId(window, &process);
        if (!thread || process != GetCurrentProcessId() || !callback || !foreground) {
            return ERROR_INVALID_PARAMETER;
        }
        {
            std::scoped_lock lock(mutex_);
            window_ = window;
            toggleKey_ = toggleKey;
            foreground_ = foreground;
            ClearPending();
            pending_.reserve(16);
        }
        // Observe removed queue messages before DispatchMessage reaches menus.
        // The nonzero thread belongs to this process; this is not a global hook.
        hook_ = SetWindowsHookExW(WH_GETMESSAGE, callback, nullptr, thread);
        return hook_ ? ERROR_SUCCESS : GetLastError();
    }

    void WindowHotkeys::Uninstall()
    {
        if (hook_) {
            UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        }
        std::scoped_lock lock(mutex_);
        window_ = nullptr;
        ClearPending();
        capture_ = {};
    }

    void WindowHotkeys::SetToggleKey(UINT key)
    {
        std::scoped_lock lock(mutex_);
        if (toggleKey_ == key) { return; }
        toggleKey_ = key;
        ClearPending();
    }

    void WindowHotkeys::BeginCapture()
    {
        std::scoped_lock lock(mutex_);
        ClearPending();
        capture_ = { CaptureStatus::Waiting };
    }

    void WindowHotkeys::CancelCapture()
    {
        std::scoped_lock lock(mutex_);
        capture_ = {};
    }

    bool WindowHotkeys::IsCapturing()
    {
        std::scoped_lock lock(mutex_);
        return capture_.status != CaptureStatus::Idle;
    }

    HotkeyCapture WindowHotkeys::TakeCapture()
    {
        std::scoped_lock lock(mutex_);
        if (capture_.status != CaptureStatus::Idle && !HasFocus()) { capture_ = { CaptureStatus::Cancelled }; }
        const auto result = capture_;
        if (result.status == CaptureStatus::Accepted || result.status == CaptureStatus::Cancelled) {
            capture_ = {};
        } else if (result.status == CaptureStatus::Rejected) {
            capture_ = { CaptureStatus::Waiting };
        }
        return result;
    }

    bool WindowHotkeys::HasFocus() const
    {
        const auto foreground = foreground_();
        return window_ && foreground && (foreground == window_ || IsChild(window_, foreground));
    }

    void WindowHotkeys::ClearPending()
    {
        pending_.clear();
        windowPresses_.fill(0);
        gamePresses_.fill(0);
    }

    void WindowHotkeys::LoseFocus()
    {
        ClearPending();
        if (capture_.status != CaptureStatus::Idle) { capture_ = { CaptureStatus::Cancelled }; }
    }

    void WindowHotkeys::CaptureKey(UINT key, std::array<unsigned, 256>& route, std::array<unsigned, 256>& other)
    {
        if (!key || key >= route.size() || IsModifierKey(key)) { return; }
        // The other route's copy of the same press is matched like a hotkey, so
        // it can neither select again nor toggle once capture has finished.
        if (other[key]) {
            --other[key];
            return;
        }
        ++route[key];
        if (capture_.status != CaptureStatus::Waiting) { return; }
        capture_ = { key == VK_ESCAPE ? CaptureStatus::Cancelled :
            IsBindableKeyboardKey(key) ? CaptureStatus::Accepted : CaptureStatus::Rejected, key };
    }

    UINT WindowHotkeys::NormalizeKey(UINT key) const
    {
        if (toggleKey_ == VK_CONTROL && (key == VK_LCONTROL || key == VK_RCONTROL)) { return VK_CONTROL; }
        if (toggleKey_ == VK_SHIFT && (key == VK_LSHIFT || key == VK_RSHIFT)) { return VK_SHIFT; }
        if (toggleKey_ == VK_MENU && (key == VK_LMENU || key == VK_RMENU)) { return VK_MENU; }
        return key;
    }

    bool WindowHotkeys::IsHotkey(UINT key) const
    {
        return key && key < windowPresses_.size() &&
            (key == toggleKey_ || key == VK_OEM_4 || key == VK_OEM_6);
    }

    UINT VirtualKeyFromGameScanCode(UINT scanCode)
    {
        if (!scanCode || scanCode > 0xFF) { return 0; }
        // E1 Pause and non-extended keypad navigation must remain distinct.
        switch (scanCode) {
        case 0x45: return VK_NUMLOCK;
        case 0xC5: return VK_PAUSE;
        case 0xB7: return VK_SNAPSHOT;
        case 0x47: return VK_NUMPAD7;
        case 0x48: return VK_NUMPAD8;
        case 0x49: return VK_NUMPAD9;
        case 0x4B: return VK_NUMPAD4;
        case 0x4C: return VK_NUMPAD5;
        case 0x4D: return VK_NUMPAD6;
        case 0x4F: return VK_NUMPAD1;
        case 0x50: return VK_NUMPAD2;
        case 0x51: return VK_NUMPAD3;
        case 0x52: return VK_NUMPAD0;
        case 0x53: return VK_DECIMAL;
        default: break;
        }
        const UINT extended = scanCode & 0x80 ? 0xE000 | (scanCode & 0x7F) : scanCode;
        return MapVirtualKeyW(extended, MAPVK_VSC_TO_VK_EX);
    }

    void WindowHotkeys::ObserveGameKeys(const std::vector<UINT>& pressedKeys)
    {
        std::scoped_lock lock(mutex_);
        if (!HasFocus()) { LoseFocus(); return; }
        gamePresses_.fill(0);
        for (auto key : pressedKeys) {
            if (capture_.status != CaptureStatus::Idle) {
                CaptureKey(key, gamePresses_, windowPresses_);
                continue;
            }
            key = NormalizeKey(key);
            if (!IsHotkey(key)) { continue; }
            if (windowPresses_[key]) { --windowPresses_[key]; }
            else {
                pending_.push_back(key);
                ++gamePresses_[key];
            }
        }
        windowPresses_.fill(0);
    }

    void WindowHotkeys::ObserveMessage(const MSG& message)
    {
        std::scoped_lock lock(mutex_);
        if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) { return; }
        if (!HasFocus()) {
            LoseFocus();
            return;
        }

        UINT key{};
        switch (message.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // Bit 30 distinguishes a fresh down from keyboard auto-repeat.
            if ((static_cast<ULONG_PTR>(message.lParam) & (1ULL << 30)) != 0) { return; }
            key = static_cast<UINT>(message.wParam);
            if (key == VK_SHIFT) {
                key = MapVirtualKeyW((message.lParam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX);
            } else if (key == VK_CONTROL) {
                key = message.lParam & (1ULL << 24) ? VK_RCONTROL : VK_LCONTROL;
            } else if (key == VK_MENU) {
                key = message.lParam & (1ULL << 24) ? VK_RMENU : VK_LMENU;
            }
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: key = VK_LBUTTON; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: key = VK_RBUTTON; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: key = VK_MBUTTON; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
            if (HIWORD(message.wParam) == XBUTTON1) { key = VK_XBUTTON1; }
            if (HIWORD(message.wParam) == XBUTTON2) { key = VK_XBUTTON2; }
            break;
        default: return;
        }

        if (capture_.status != CaptureStatus::Idle) {
            // Mouse clicks still reach the menu, such as its Cancel button.
            if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
                CaptureKey(key, windowPresses_, gamePresses_);
            }
            return;
        }
        key = NormalizeKey(key);
        if (!IsHotkey(key)) { return; }
        if (gamePresses_[key]) { --gamePresses_[key]; }
        else {
            pending_.push_back(key);
            ++windowPresses_[key];
        }
    }

    LRESULT WindowHotkeys::ForwardMessage(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code >= 0 && wParam == PM_REMOVE && lParam) { ObserveMessage(*reinterpret_cast<const MSG*>(lParam)); }
        // Always preserve both the hook chain and the original window message.
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    std::vector<UINT> WindowHotkeys::TakePending()
    {
        std::scoped_lock lock(mutex_);
        if (!HasFocus()) { ClearPending(); }
        const auto result = pending_;
        pending_.clear();
        return result;
    }
}
