#include "OverlayHotkeys.h"
#include "OverlayGameInput.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>

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

    void ExpectCapture(CaptureStatus status, UINT key, const char* message)
    {
        const auto capture = keys.TakeCapture();
        Require(capture.status == status && capture.key == key, message);
    }

    struct IniFixture
    {
        std::map<std::string, long> values;
        bool hex{};
        long GetLongValue(const char* section, const char* key, long fallback) const
        {
            const auto found = values.find(std::string(section) + "/" + key);
            return found == values.end() ? fallback : found->second;
        }
        void SetLongValue(const char* section, const char* key, long value, const char*, bool useHex)
        {
            values[std::string(section) + "/" + key] = value;
            hex = useHex;
        }
    };

    void Capture()
    {
        Reset();
        keys.BeginCapture();
        Post(VK_F10);
        Expect({}, "a captured key does not queue a hotkey action");
        ExpectCapture(CaptureStatus::Accepted, VK_F10, "window route selects the key");
        ExpectCapture(CaptureStatus::Idle, 0, "the result is taken once");
        keys.ObserveGameKeys({VK_F10});
        Expect({}, "Skyrim's copy of the selected key cannot toggle after capture ends");
        keys.ObserveGameKeys({});
        Post(VK_F10);
        Expect({VK_F10}, "hotkeys resume after capture");

        Reset();
        keys.BeginCapture();
        keys.ObserveGameKeys({VK_F10});
        ExpectCapture(CaptureStatus::Accepted, VK_F10, "game route selects under exclusive input");
        Post(VK_F10);
        Expect({}, "the Windows copy of the selected key cannot toggle after capture ends");

        Reset();
        keys.BeginCapture();
        Post(VK_F6); Post(VK_F10); keys.ObserveGameKeys({VK_F6, VK_F10});
        ExpectCapture(CaptureStatus::Accepted, VK_F6, "the first press wins");
        Expect({}, "presses after the selection stay inside capture");

        Reset();
        keys.BeginCapture();
        Post(VK_SHIFT); keys.ObserveGameKeys({VK_LSHIFT, VK_LCONTROL, VK_F8});
        ExpectCapture(CaptureStatus::Accepted, VK_F8, "modifiers are skipped so Shift+F8 selects F8");

        Reset();
        keys.BeginCapture();
        Post(0, WM_LBUTTONDOWN);
        Post(VK_F7, WM_KEYDOWN, LPARAM(1ULL << 30) | 1);
        Require(keys.IsCapturing(), "clicks and auto-repeat leave capture waiting");
        ExpectCapture(CaptureStatus::Waiting, 0, "clicks and auto-repeat select nothing");
        Post(VK_ESCAPE);
        ExpectCapture(CaptureStatus::Cancelled, VK_ESCAPE, "Escape cancels");
        Require(!keys.IsCapturing(), "cancel ends capture");

        Reset();
        keys.BeginCapture();
        Post(VK_LWIN);
        ExpectCapture(CaptureStatus::Rejected, VK_LWIN, "Windows key rejected");
        Require(keys.IsCapturing(), "rejection keeps waiting for another key");
        keys.ObserveGameKeys({VK_LWIN});
        ExpectCapture(CaptureStatus::Waiting, 0, "the rejected key's second copy is not reported twice");
        keys.ObserveGameKeys({VK_F9});
        ExpectCapture(CaptureStatus::Accepted, VK_F9, "a valid key after rejection is selected");

        Reset();
        keys.BeginCapture();
        focused = nullptr;
        ExpectCapture(CaptureStatus::Cancelled, 0, "focus loss cancels capture");
        focused = window;
        keys.BeginCapture();
        focused = nullptr;
        Post(VK_F9);
        focused = window;
        ExpectCapture(CaptureStatus::Cancelled, 0, "a press without focus cancels rather than selects");

        Reset();
        Post(VK_F10);
        keys.BeginCapture();
        keys.CancelCapture();
        Expect({}, "starting capture discards an undrained hotkey");
        Post(VK_F10);
        Expect({VK_F10}, "a cancelled capture restores hotkeys");

        Reset();
        Post(VK_F10);
        keys.SetToggleKey(VK_F7);
        Expect({}, "rebinding discards a press of the old key");
        Post(VK_F10); keys.ObserveGameKeys({VK_F10});
        Expect({}, "the old key no longer toggles");
        Post(VK_F7); keys.ObserveGameKeys({VK_F7});
        Expect({VK_F7}, "the new key toggles on both routes without reinstalling the hook");
    }

    void Policy()
    {
        for (UINT key : {UINT(VK_F10), UINT(VK_F24), UINT(VK_END), UINT(VK_INSERT), UINT('A'), UINT('7'),
                         UINT(VK_NUMPAD1), UINT(VK_OEM_4), UINT(VK_OEM_3), UINT(VK_OEM_102), UINT(VK_PAUSE)}) {
            Require(IsBindableKeyboardKey(key), "keyboard key can be picked");
        }
        for (UINT key : {UINT(VK_ESCAPE), UINT(VK_LWIN), UINT(VK_APPS), UINT(VK_CAPITAL), UINT(VK_SNAPSHOT),
                         UINT(VK_LBUTTON), UINT(VK_XBUTTON1), UINT(VK_SHIFT), UINT(VK_RMENU), UINT(VK_VOLUME_UP)}) {
            Require(!IsBindableKeyboardKey(key), "escape, system, modifier, mouse and media keys cannot be picked");
        }
        for (UINT key : {UINT(VK_END), UINT(VK_F1), UINT(VK_F10), UINT(VK_INSERT), UINT(VK_PRIOR), UINT(VK_PAUSE)}) {
            Require(CanToggleWhileEditing(key) && ActionsForHotkey(key, key, true, false).toggle,
                "non-editing keys close the menu during text editing");
        }
        for (UINT key : {UINT('2'), UINT('A'), UINT(VK_HOME), UINT(VK_DELETE), UINT(VK_BACK), UINT(VK_SPACE), UINT(VK_OEM_4)}) {
            Require(!CanToggleWhileEditing(key) && !ActionsForHotkey(key, key, true, false).toggle,
                "typing and editing keys keep editing the active field");
        }
#if !defined(TRP_NO_NEURAL_RENDERING)
        Require(ReservedForNRHotkeys(VK_OEM_4, true) && ReservedForNRHotkeys(VK_OEM_6, true), "enabled NR shortcuts reserve brackets");
#endif
        Require(!ReservedForNRHotkeys(VK_OEM_4, false) && !ReservedForNRHotkeys(VK_F10, true), "brackets are free when NR shortcuts are off");

        Require(HotkeyName(VK_F10) == "F10" && HotkeyName(VK_F24) == "F24", "function key names");
        Require(HotkeyName(VK_XBUTTON1) == "Mouse 4", "existing mouse binding name");
        Require(HotkeyName(VK_END) != HotkeyName(VK_NUMPAD1), "End is not shown as Num 1");
        Require(HotkeyName(VK_END).rfind("Key 0x", 0) != 0 && HotkeyName(VK_NEXT) != HotkeyName(VK_NUMPAD3),
            "navigation keys have layout names");

        IniFixture ini;
        Require(LoadMenuHotkey(ini) == VK_END, "missing ToggleOverlay defaults to End");
        for (long invalid : {0L, -1L, 0xFFL, 0x1FFL}) {
            ini.values["Hotkeys/ToggleOverlay"] = invalid;
            Require(LoadMenuHotkey(ini) == VK_END, "invalid ToggleOverlay falls back to End");
        }
        ini.values["Hotkeys/ToggleOverlay"] = VK_XBUTTON1;
        Require(LoadMenuHotkey(ini) == VK_XBUTTON1, "existing INI mouse binding preserved");
        StoreMenuHotkey(ini, VK_F10);
        Require(ini.hex && LoadMenuHotkey(ini) == VK_F10, "saved key round trips as hex");
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

    Capture();
    Policy();

    Require(VirtualKeyFromGameScanCode(0x44) == VK_F10, "F10 scan code");
    Require(VirtualKeyFromGameScanCode(0xCF) == VK_END, "extended End is not keypad 1");
    Require(VirtualKeyFromGameScanCode(0xD1) == VK_NEXT, "Page Down is not End");
    Require(VirtualKeyFromGameScanCode(0x4F) == VK_NUMPAD1, "keypad identity");
    Require(VirtualKeyFromGameScanCode(0x9D) == VK_RCONTROL, "right control scan code");
    Require(VirtualKeyFromGameScanCode(0xC5) == VK_PAUSE, "Pause special scan code");
    Require(VirtualKeyFromGameScanCode(0x45) == VK_NUMLOCK, "Num Lock special scan code");
    Require(VirtualKeyFromGameScanCode(256) == 0, "mouse IDs cannot become keyboard keys");
    for (bool enabled : {false, true}) {
        Require(ActionsForHotkey(VK_END, VK_END, true, enabled).toggle, "End still closes during editing");
        Require(ActionsForHotkey(VK_F10, VK_F10, true, enabled).toggle, "F10 closes during editing");
        Require(!ActionsForHotkey('2', '2', true, enabled).toggle, "numeric binding remains typing during editing");
        Require(ActionsForHotkey(VK_OEM_4, VK_F10, true, enabled).neuralState == -1, "editing protects NR off");
        Require(ActionsForHotkey(VK_OEM_6, VK_F10, true, enabled).neuralState == -1, "editing protects NR on");
        Require(ActionsForHotkey(VK_OEM_COMMA, VK_END, false, enabled).neuralState == -1, "comma remains unbound");
    }
    for (UINT bracket : {VK_OEM_4, VK_OEM_6}) {
        Require(ActionsForHotkey(bracket, VK_END, false, false).neuralState == -1,
            "disabled NR shortcuts cannot change the session");
        const auto rebound = ActionsForHotkey(bracket, bracket, false, false);
        Require(rebound.toggle && rebound.neuralState == -1, "bracket can still be the configured menu key");
    }
    Require(ActionsForHotkey(VK_OEM_4, VK_END, false, true).neuralState == 0, "opt-in NR off");
    Require(ActionsForHotkey(VK_OEM_6, VK_END, false, true).neuralState == 1, "opt-in NR on");
    keys.Uninstall();
    keys.ObserveGameKeys({VK_F10});
    Expect({}, "uninstalled observer rejects game input");
    UnhookWindowsHookEx(next);
    DestroyWindow(window);
    std::puts("PASS production hotkey routes, batch matching, capture, rebinding, focus, scan codes and editing policy");
}
