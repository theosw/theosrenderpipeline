#include "OverlayHotkeys.h"
#include "OverlayGameInput.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace TheosRenderPipeline::Overlay;
namespace
{
    WindowHotkeys keys;
    HWND focused{}, window{};
    unsigned forwarded{};
    bool consumeMessages{};
    HWND WINAPI Foreground() { return focused; }
    LRESULT CALLBACK Observer(int code, WPARAM wp, LPARAM lp) { return keys.ForwardMessage(code, wp, lp); }
    LRESULT CALLBACK Next(int code, WPARAM wp, LPARAM lp)
    {
        if (code >= 0 && wp == PM_REMOVE) {
            ++forwarded;
            if (consumeMessages && lp) { reinterpret_cast<MSG*>(lp)->message = WM_NULL; }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    void Require(bool condition, const char* message)
    {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    }
    void Pump()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    }
    void Post(UINT key, UINT type = WM_KEYDOWN, LPARAM flags = 1)
    {
        const auto before = forwarded;
        Require(PostMessageW(window, type, key, flags) != FALSE, "post key");
        Pump();
        Require(forwarded > before, "every message reaches the downstream hook");
    }
    void Expect(std::initializer_list<UINT> expected, const char* message)
    {
        Require(keys.TakePending() == std::vector<UINT>(expected), message);
    }
    void Reset(UINT toggle = VK_F10)
    {
        keys.Uninstall();
        focused = window;
        Require(keys.Install(window, toggle, Observer, Foreground) == ERROR_SUCCESS, "install production hook");
        Pump();
    }

    struct EventFixture
    {
        EventFixture* next{};
        bool button{true}, down{true};
        int device{};
        UINT scan{0x44};
        const EventFixture* AsButtonEvent() const { return button ? this : nullptr; }
        bool IsDown() const { return down; }
        int GetDevice() const { return device; }
        UINT GetIDCode() const { return scan; }
    };
}

int main()
{
    window = CreateWindowExW(0, L"STATIC", L"TRP input fixture", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "create message-only window");
    const auto next = SetWindowsHookExW(WH_GETMESSAGE, Next, nullptr, GetCurrentThreadId());
    Require(next != nullptr, "downstream hook");

    EventFixture initial, held, mouse, character, invalid;
    initial.next = &held; held.next = &mouse; mouse.next = &character; character.next = &invalid;
    held.down = false; mouse.device = 1; character.button = false; invalid.scan = 256;
    Require(CollectGameHotkeyPresses(&initial, 0) == std::vector<UINT>{VK_F10},
        "production collector selects initial keyboard presses, not held/mouse/character/invalid events");
    Require(initial.next == &held && held.next == &mouse && mouse.next == &character && character.next == &invalid,
        "game event chain remains unchanged for other listeners");
    Require(CollectGameHotkeyPresses(static_cast<EventFixture*>(nullptr), 0).empty(), "null game input batch");

    Reset();
    Require(PostMessageW(window, WM_KEYDOWN, VK_F10, 1) != FALSE, "post before non-removing peek");
    MSG peek{};
    Require(PeekMessageW(&peek, window, WM_KEYDOWN, WM_KEYDOWN, PM_NOREMOVE) != FALSE, "peek without removal");
    Expect({}, "non-removing peeks do not queue input");
    Pump(); keys.ObserveGameKeys({VK_F10});
    Expect({VK_F10}, "removed message queues exactly once");

    Reset();
    consumeMessages = true;
    Post(VK_F10); keys.ObserveGameKeys({});
    Expect({VK_F10}, "downstream menu suppression cannot hide an observed Windows press");
    consumeMessages = false;

    Reset();
    keys.ObserveGameKeys({VK_F10});
    Expect({VK_F10}, "exclusive-input game route works without a Windows message");
    keys.ObserveGameKeys({});
    keys.ObserveGameKeys({VK_F10});
    Expect({VK_F10}, "subsequent game-only press works");

    Reset();
    Post(VK_F10, WM_SYSKEYDOWN);
    Post(VK_F10, WM_KEYUP);
    Expect({VK_F10}, "short window press is retained until Present");
    keys.ObserveGameKeys({VK_F10});
    Expect({}, "later game copy does not toggle again after Present drained Windows");

    Reset();
    keys.ObserveGameKeys({VK_F10});
    Expect({VK_F10}, "game-first press delivered");
    Post(VK_F10, WM_SYSKEYDOWN);
    Expect({}, "window copy after Present does not toggle again");
    keys.ObserveGameKeys({});
    Post(VK_F10);
    Expect({VK_F10}, "completed matching interval does not consume a new Windows press");

    Reset();
    Post(VK_F10); Post(VK_F10, WM_KEYUP); Post(VK_F10);
    keys.ObserveGameKeys({VK_F10, VK_F10});
    Expect({VK_F10, VK_F10}, "two short presses remain two, not one per frame");
    keys.ObserveGameKeys({});
    Post(VK_F10, WM_KEYDOWN, LPARAM(1ULL << 30) | 1);
    Expect({}, "Windows auto-repeat rejected");

    Reset();
    // A competing menu can omit Skyrim's copy. Empty batches retire unmatched
    // credits without requiring a fake key-up or delaying the Windows action.
    Post(VK_F10); keys.ObserveGameKeys({});
    Post(VK_F10); keys.ObserveGameKeys({});
    Expect({VK_F10, VK_F10}, "Windows route survives consumed game events");
    keys.ObserveGameKeys({VK_F10});
    Expect({VK_F10}, "game fallback recovers after window-only input batches");
    keys.ObserveGameKeys({});
    Post(VK_F10);
    Expect({VK_F10}, "window route recovers after a game-only batch");

    Reset();
    Post('A'); keys.ObserveGameKeys({'A', VK_END, 0, 0xFFFF});
    Expect({}, "unbound and invalid keys ignored");
    Post(VK_OEM_4); keys.ObserveGameKeys({VK_OEM_4, VK_OEM_6});
    Expect({VK_OEM_4, VK_OEM_6}, "NR shortcuts deduplicate independently");

    Reset();
    focused = nullptr;
    Post(VK_F10); keys.ObserveGameKeys({VK_F10});
    Expect({}, "both routes reject background input");
    focused = window;
    keys.ObserveGameKeys({VK_F10});
    focused = nullptr;
    Expect({}, "focus loss before Present discards action and matching credits");
    focused = window;
    Post(VK_F10); keys.ObserveGameKeys({VK_F10});
    Expect({VK_F10}, "focus recovery does not inherit stale credits");

    Reset(VK_CONTROL);
    Post(VK_CONTROL); keys.ObserveGameKeys({VK_LCONTROL});
    Expect({VK_CONTROL}, "generic modifier binding matches both routes");
    Reset(VK_RCONTROL);
    Post(VK_CONTROL, WM_KEYDOWN, LPARAM(1ULL << 24) | 1);
    keys.ObserveGameKeys({VK_RCONTROL});
    Expect({VK_RCONTROL}, "right modifier binding keeps extended identity");
    Reset(VK_XBUTTON1);
    Post(MAKEWPARAM(0, XBUTTON1), WM_XBUTTONDOWN);
    Expect({VK_XBUTTON1}, "existing mouse binding preserved");

    Require(VirtualKeyFromGameScanCode(0x44) == VK_F10, "F10 scan code");
    Require(VirtualKeyFromGameScanCode(0xCF) == VK_END, "extended End is not keypad 1");
    Require(VirtualKeyFromGameScanCode(0xD1) == VK_NEXT, "Page Down is not End");
    Require(VirtualKeyFromGameScanCode(0x4F) == VK_NUMPAD1, "keypad identity");
    Require(VirtualKeyFromGameScanCode(0x9D) == VK_RCONTROL, "right control scan code");
    Require(VirtualKeyFromGameScanCode(0xC5) == VK_PAUSE, "Pause special scan code");
    Require(VirtualKeyFromGameScanCode(0x45) == VK_NUMLOCK, "Num Lock special scan code");
    Require(VirtualKeyFromGameScanCode(256) == 0, "mouse IDs cannot become keyboard keys");
    Require(ActionsForHotkey(VK_END, VK_END, true).toggle, "End still closes during editing");
    Require(!ActionsForHotkey('2', '2', true).toggle, "numeric binding remains typing during editing");
    Require(ActionsForHotkey(VK_OEM_4, VK_F10, true).neuralState == -1, "editing protects NR shortcut");
    keys.Uninstall();
    keys.ObserveGameKeys({VK_F10});
    Expect({}, "uninstalled observer rejects game input");
    UnhookWindowsHookEx(next);
    DestroyWindow(window);
    std::puts("PASS production hotkey routes, batch matching, focus, scan codes and editing policy");
}
