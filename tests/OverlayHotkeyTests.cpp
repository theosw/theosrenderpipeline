#include "OverlayHotkeys.h"
#include <SimpleIni.h>
#include <algorithm>
#include "RendererSettings.h"
#include <cstdio>
#include <cstdlib>
#define TRP_REQUIRE(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #expression); std::exit(1); \
} } while (false)
#include <atomic>
#include <future>
#include <thread>

using namespace TheosRenderPipeline::Overlay;
namespace
{
    std::atomic<HWND> foreground{};
    std::atomic<unsigned> forwardedDowns{}, deliveredDowns{};
    WindowHotkeys* hotkeys{};
    HANDLE drained{};
    HWND child{}, other{};
    constexpr UINT kDrain = WM_APP + 1;
    constexpr UINT kStop = WM_APP + 2;
    constexpr LPARAM kEndDown = 1 | (0x4F << 16) | (1 << 24);
    constexpr LPARAM kRepeat = static_cast<LPARAM>(1ULL << 30);
    constexpr LPARAM kRelease = static_cast<LPARAM>(3ULL << 30);

    HWND WINAPI TestForeground() { return foreground.load(); }
    LRESULT CALLBACK Observer(int code, WPARAM wParam, LPARAM lParam)
    {
        return hotkeys->ForwardMessage(code, wParam, lParam);
    }
    LRESULT CALLBACK NextObserver(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code >= 0 && wParam == PM_REMOVE && lParam) {
            const auto& message = *reinterpret_cast<const MSG*>(lParam);
            if (message.message == WM_KEYDOWN) { ++forwardedDowns; }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    LRESULT CALLBACK MenuWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            ++deliveredDowns;
            return 0x51; // A downstream menu consumes input; no game event is produced.
        }
        if (message == kDrain) { SetEvent(drained); return 0; }
        if (message == kStop) {
            DestroyWindow(child);
            DestroyWindow(other);
            DestroyWindow(window);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    void Post(HWND window, UINT message, WPARAM key = VK_END, LPARAM flags = kEndDown)
    {
        TRP_REQUIRE(PostMessageW(window, message, key, flags));
    }
    void Drain(HWND window)
    {
        TRP_REQUIRE(ResetEvent(drained));
        Post(window, kDrain, 0, 0);
        TRP_REQUIRE(WaitForSingleObject(drained, 5000) == WAIT_OBJECT_0);
    }
}

int main()
{
    // Hidden fixture windows never take focus or send input to another process.
    WindowHotkeys queue;
    hotkeys = &queue;
    drained = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    TRP_REQUIRE(drained);
    std::promise<HWND> windowReady;
    auto ready = windowReady.get_future();
    std::thread windowThread([&] {
        WNDCLASSW windowClass{};
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpfnWndProc = MenuWindow;
        windowClass.lpszClassName = L"TRPHotkeyFixture";
        windowClass.style = CS_DBLCLKS;
        TRP_REQUIRE(RegisterClassW(&windowClass));
        const auto window = CreateWindowExW(0, windowClass.lpszClassName, L"fixture", WS_POPUP,
            0, 0, 10, 10, nullptr, nullptr, windowClass.hInstance, nullptr);
        child = CreateWindowExW(0, windowClass.lpszClassName, L"child", WS_CHILD,
            0, 0, 5, 5, window, nullptr, windowClass.hInstance, nullptr);
        other = CreateWindowExW(0, windowClass.lpszClassName, L"unrelated", WS_POPUP,
            0, 0, 10, 10, nullptr, nullptr, windowClass.hInstance, nullptr);
        TRP_REQUIRE(window && child && other);
        windowReady.set_value(window);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { DispatchMessageW(&message); }
        TRP_REQUIRE(UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance));
    });
    const auto window = ready.get();
    foreground = window;
    const auto next = SetWindowsHookExW(WH_GETMESSAGE, NextObserver, nullptr,
        GetWindowThreadProcessId(window, nullptr));
    TRP_REQUIRE(next);
    TRP_REQUIRE(queue.Install(nullptr, VK_END, Observer, TestForeground) == ERROR_INVALID_PARAMETER);
    TRP_REQUIRE(queue.Install(window, VK_END, nullptr, TestForeground) == ERROR_INVALID_PARAMETER);
    TRP_REQUIRE(queue.Install(window, VK_END, Observer, TestForeground) == ERROR_SUCCESS);
    TRP_REQUIRE(queue.Install(window, VK_END, Observer, TestForeground) == ERROR_ALREADY_EXISTS);

    // A down/up pair completes on the window thread before Present drains it.
    // Both the later hook and the consuming window procedure still receive it.
    const auto beforeForward = forwardedDowns.load();
    const auto beforeDelivery = deliveredDowns.load();
    Post(window, WM_KEYDOWN);
    Post(window, WM_KEYUP, VK_END, kEndDown | kRelease);
    Drain(window);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_END });
    TRP_REQUIRE(queue.TakePending().empty());
    TRP_REQUIRE(forwardedDowns == beforeForward + 1 && deliveredDowns == beforeDelivery + 1);
    Post(window, WM_KEYDOWN);
    for (int i = 0; i < 100; ++i) { Post(window, WM_KEYDOWN, VK_END, kEndDown | kRepeat); }
    Post(window, WM_KEYUP, VK_END, kEndDown | kRelease);
    Post(window, WM_KEYDOWN);
    Post(window, WM_KEYUP, VK_END, kEndDown | kRelease);
    Drain(window);
    TRP_REQUIRE((queue.TakePending() == std::vector<UINT>{ VK_END, VK_END }));
    TRP_REQUIRE(SendMessageW(window, WM_KEYDOWN, VK_END, kEndDown) == 0x51);
    // Sent messages bypass the queue; the observer leaves their return unchanged.
    TRP_REQUIRE(queue.TakePending().empty());

    Post(window, WM_KEYDOWN, VK_OEM_4);
    Post(window, WM_KEYDOWN, 'A');
    Post(window, WM_KEYDOWN);
    Post(window, WM_KEYDOWN, VK_OEM_6);
    Drain(window);
    TRP_REQUIRE((queue.TakePending() == std::vector<UINT>{ VK_OEM_4, VK_END, VK_OEM_6 }));
    Post(other, WM_KEYDOWN);
    Drain(window);
    TRP_REQUIRE(queue.TakePending().empty());
    foreground = child;
    Post(child, WM_KEYDOWN);
    Drain(window);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_END });

    foreground = window;
    Post(window, WM_KEYDOWN);
    Drain(window);
    // Focus loss observed by the queue clears input even if focus returns before Present.
    foreground = nullptr;
    Post(window, WM_KEYUP, VK_END, kEndDown | kRelease);
    Drain(window);
    foreground = window;
    TRP_REQUIRE(queue.TakePending().empty());
    foreground = nullptr;
    Post(window, WM_KEYDOWN);
    Drain(window);
    foreground = window;
    TRP_REQUIRE(queue.TakePending().empty());
    Post(window, WM_KEYDOWN);
    Drain(window);
    foreground = nullptr;
    TRP_REQUIRE(queue.TakePending().empty());
    foreground = window;

    const auto bind = [&](UINT key) {
        queue.SetToggleKey(key);
    };
    bind(VK_F8);
    Post(window, WM_KEYDOWN);
    Post(window, WM_KEYDOWN, VK_F8);
    Drain(window);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_F8 });
    bind(VK_CONTROL);
    Post(window, WM_KEYDOWN, VK_CONTROL, 1 | (1 << 24));
    Drain(window);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_CONTROL });
    bind(VK_RCONTROL);
    Post(window, WM_KEYDOWN, VK_CONTROL, 1);
    Post(window, WM_KEYDOWN, VK_CONTROL, 1 | (1 << 24));
    Drain(window);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_RCONTROL });
    bind(VK_RSHIFT);
    Post(window, WM_KEYDOWN, VK_SHIFT, 1 | (0x36 << 16));
    Drain(window);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_RSHIFT });
    bind(VK_RMENU);
    Post(window, WM_SYSKEYDOWN, VK_MENU, 1 | (1 << 24));
    Drain(window);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_RMENU });
    bind(VK_XBUTTON1);
    Post(window, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), 0);
    Post(window, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    Post(window, WM_XBUTTONDBLCLK, MAKEWPARAM(0, XBUTTON1), 0);
    Drain(window);
    TRP_REQUIRE((queue.TakePending() == std::vector<UINT>{ VK_XBUTTON1, VK_XBUTTON1 }));
    bind(VK_NUMPAD1);
    Post(window, WM_KEYDOWN, VK_END);
    Post(window, WM_KEYDOWN, VK_NUMPAD1);
    Drain(window);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_NUMPAD1 });

    // Apply a new binding without reinstalling the observer. Stale actions are
    // dropped on change, but an unchanged binding must preserve queued presses.
    bind(VK_END);
    Post(window, WM_KEYDOWN);
    Drain(window);
    bind(VK_F10);
    TRP_REQUIRE(queue.TakePending().empty());
    Post(window, WM_KEYDOWN);
    Post(window, WM_KEYDOWN, VK_F10);
    Drain(window);
    bind(VK_F10);
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_F10 });

    Post(window, WM_KEYDOWN, VK_F10);
    Drain(window);
    queue.BeginCapture();
    TRP_REQUIRE(queue.TakePending().empty());
    TRP_REQUIRE(queue.IsCapturing());
    // Opening mouse click, key releases and repeats never become bindings.
    Post(window, WM_LBUTTONUP, 0, 0);
    Post(window, WM_LBUTTONDOWN, 0, 0);
    Post(window, WM_KEYUP, VK_F2, kRelease);
    Post(window, WM_KEYDOWN, VK_F2, kRepeat);
    Drain(window);
    TRP_REQUIRE(queue.TakeCapture().status == CaptureStatus::Waiting);
    // Capture the currently active toggle, then another NR press before Present.
    // Neither becomes a toggle or NR action, including after result consumption.
    Post(window, WM_KEYDOWN, VK_F10);
    Post(window, WM_KEYDOWN, VK_OEM_6);
    Drain(window);
    const auto selected = queue.TakeCapture();
    TRP_REQUIRE(selected.status == CaptureStatus::Accepted && selected.key == VK_F10);
    TRP_REQUIRE(!queue.IsCapturing() && queue.TakePending().empty());
    TRP_REQUIRE(queue.TakeCapture().status == CaptureStatus::Idle);

    queue.BeginCapture();
    Post(window, WM_KEYDOWN, VK_ESCAPE);
    Post(window, WM_KEYDOWN, VK_F10);
    Drain(window);
    TRP_REQUIRE(queue.TakeCapture().status == CaptureStatus::Cancelled);
    TRP_REQUIRE(queue.TakePending().empty() && !queue.IsCapturing());

    for (const auto invalid : {VK_OEM_4, VK_OEM_6, VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN}) {
        queue.BeginCapture();
        Post(window, WM_KEYDOWN, invalid, 1 | (0x2A << 16));
        Drain(window);
        TRP_REQUIRE(queue.TakeCapture().status == CaptureStatus::Rejected);
        TRP_REQUIRE(queue.IsCapturing() && queue.TakePending().empty());
        Post(window, WM_KEYDOWN, 'K');
        Drain(window);
        const auto retry = queue.TakeCapture();
        TRP_REQUIRE(retry.status == CaptureStatus::Accepted && retry.key == 'K');
    }
    queue.BeginCapture();
    foreground = nullptr;
    TRP_REQUIRE(queue.TakeCapture().status == CaptureStatus::Cancelled);
    foreground = window;
    queue.BeginCapture();
    foreground = nullptr;
    Post(window, WM_KEYUP, VK_F10, kRelease);
    Drain(window);
    foreground = window;
    TRP_REQUIRE(queue.TakeCapture().status == CaptureStatus::Cancelled);
    queue.BeginCapture();
    Post(window, WM_KEYDOWN, 'K');
    Drain(window);
    queue.CancelCapture(); // Discard/close cancels even an unacknowledged result.
    TRP_REQUIRE(queue.TakeCapture().status == CaptureStatus::Idle);
    TRP_REQUIRE(queue.TakePending().empty());

    for (UINT key = VK_F1; key <= VK_F24; ++key) {
        TRP_REQUIRE(IsBindableKeyboardKey(key));
        TRP_REQUIRE(ActionsForHotkey(key, key, true).toggle);
    }
    for (const auto key : std::array<int, 7>{'2', 'K', VK_HOME, VK_BACK, VK_RETURN, VK_LEFT, VK_NUMPAD2}) {
        TRP_REQUIRE(IsBindableKeyboardKey(key));
        TRP_REQUIRE(!ActionsForHotkey(key, key, true).toggle);
        TRP_REQUIRE(ActionsForHotkey(key, key, false).toggle);
    }
    TRP_REQUIRE(!IsBindableKeyboardKey(VK_LBUTTON));
    TRP_REQUIRE(!IsBindableKeyboardKey(VK_ESCAPE));
    TRP_REQUIRE(!IsBindableKeyboardKey(0));
    TRP_REQUIRE(!IsBindableKeyboardKey(0xFF));
    TRP_REQUIRE(HotkeyName(VK_F10) == "F10");
    TRP_REQUIRE(HotkeyName(VK_NUMPAD2) == "Num 2");
    TRP_REQUIRE(HotkeyName(VK_END) != HotkeyName(VK_NUMPAD1));

    // Exercise the production INI helpers, including a serialized restart and
    // unrelated settings. Applying a draft alone must not alter saved defaults.
    CSimpleIniA ini;
    TRP_REQUIRE(LoadMenuHotkey(ini) == VK_END);
    ini.SetValue("Unrelated", "Keep", "preserved");
    StoreMenuHotkey(ini, VK_F10);
    TheosRenderPipeline::RendererSettingsDraft current;
    current.valid = true;
    current.menuHotkey = LoadMenuHotkey(ini);
    auto draft = current;
    draft.menuHotkey = 'K';
    TRP_REQUIRE(TheosRenderPipeline::CountRendererSettingsChanges(draft, current) == 1);
    TRP_REQUIRE(LoadMenuHotkey(ini) == VK_F10);
    const TheosRenderPipeline::RendererSettingsCapabilities capabilities{true, true, true, false};
    TRP_REQUIRE(TheosRenderPipeline::ValidateRendererSettings(draft, capabilities) == nullptr);
    auto discarded = current;
    TRP_REQUIRE(TheosRenderPipeline::CountRendererSettingsChanges(discarded, current) == 0);
    StoreMenuHotkey(ini, draft.menuHotkey);
    std::string serialized;
    TRP_REQUIRE(ini.Save(serialized) >= 0);
    CSimpleIniA restarted;
    TRP_REQUIRE(restarted.LoadData(serialized) >= 0);
    TRP_REQUIRE(LoadMenuHotkey(restarted) == 'K');
    TRP_REQUIRE(std::string(restarted.GetValue("Unrelated", "Keep")) == "preserved");
    StoreMenuHotkey(restarted, VK_END);
    TRP_REQUIRE(LoadMenuHotkey(restarted) == VK_END);
    StoreMenuHotkey(restarted, VK_XBUTTON1); // Existing INI-only bindings survive.
    TRP_REQUIRE(LoadMenuHotkey(restarted) == VK_XBUTTON1);
    for (const auto invalid : {0, -1, 255, 65536}) {
        restarted.SetLongValue("Hotkeys", "ToggleOverlay", invalid);
        TRP_REQUIRE(LoadMenuHotkey(restarted) == VK_END);
        draft.menuHotkey = invalid;
        TRP_REQUIRE(TheosRenderPipeline::ValidateRendererSettings(draft, capabilities) != nullptr);
    }

    TRP_REQUIRE(ActionsForHotkey(VK_END, VK_END, true).toggle);
    TRP_REQUIRE(!ActionsForHotkey('2', '2', true).toggle);
    TRP_REQUIRE(ActionsForHotkey('2', '2', false).toggle);
    TRP_REQUIRE(ActionsForHotkey(VK_OEM_4, VK_END, false).neuralState == 0);
    TRP_REQUIRE(ActionsForHotkey(VK_OEM_6, VK_END, false).neuralState == 1);
    TRP_REQUIRE(ActionsForHotkey(VK_OEM_6, VK_END, true).neuralState == -1);
    queue.Uninstall();
    Post(window, WM_KEYDOWN);
    Drain(window);
    TRP_REQUIRE(queue.TakePending().empty());
    TRP_REQUIRE(UnhookWindowsHookEx(next));

    // Repeated non-removing peeks must not toggle; removing the same message
    // produces exactly one request. This window belongs to the current thread.
    const auto localWindow = CreateWindowExW(0, L"TRPHotkeyFixture", L"peek fixture", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    TRP_REQUIRE(localWindow);
    foreground = localWindow;
    TRP_REQUIRE(queue.Install(localWindow, VK_END, Observer, TestForeground) == ERROR_SUCCESS);
    Post(localWindow, WM_KEYDOWN);
    Post(localWindow, WM_KEYUP, VK_END, kEndDown | kRelease);
    MSG peek{};
    TRP_REQUIRE(PeekMessageW(&peek, localWindow, WM_KEYDOWN, WM_KEYUP, PM_NOREMOVE));
    TRP_REQUIRE(PeekMessageW(&peek, localWindow, WM_KEYDOWN, WM_KEYUP, PM_NOREMOVE));
    TRP_REQUIRE(queue.TakePending().empty());
    while (PeekMessageW(&peek, localWindow, WM_KEYDOWN, WM_KEYUP, PM_REMOVE)) { DispatchMessageW(&peek); }
    TRP_REQUIRE(queue.TakePending() == std::vector<UINT>{ VK_END });
    queue.Uninstall();
    TRP_REQUIRE(DestroyWindow(localWindow));
    Post(window, kStop, 0, 0);
    windowThread.join();
    TRP_REQUIRE(CloseHandle(drained));
    std::puts("Window hotkeys: cross-thread message capture, consuming menu, forwarding, repeats, focus and bindings passed.");
}
