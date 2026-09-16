#include "OverlayHotkeys.h"

namespace TheosRenderPipeline::Overlay
{
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
            pending_.clear();
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
        pending_.clear();
    }

    bool WindowHotkeys::HasFocus() const
    {
        const auto foreground = foreground_();
        return window_ && foreground && (foreground == window_ || IsChild(window_, foreground));
    }

    void WindowHotkeys::ObserveMessage(const MSG& message)
    {
        std::scoped_lock lock(mutex_);
        if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) { return; }
        if (!HasFocus()) {
            pending_.clear();
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

        if (!key) { return; }
        if (toggleKey_ == VK_CONTROL && (key == VK_LCONTROL || key == VK_RCONTROL)) { key = VK_CONTROL; }
        if (toggleKey_ == VK_SHIFT && (key == VK_LSHIFT || key == VK_RSHIFT)) { key = VK_SHIFT; }
        if (toggleKey_ == VK_MENU && (key == VK_LMENU || key == VK_RMENU)) { key = VK_MENU; }
        if (key == toggleKey_ || key == VK_OEM_4 || key == VK_OEM_6) { pending_.push_back(key); }
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
        if (!HasFocus()) { pending_.clear(); }
        const auto result = pending_;
        pending_.clear();
        return result;
    }
}
