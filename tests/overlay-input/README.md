# Overlay input checks

The fixture compiles the production `OverlayHotkeys.cpp`. It uses a hidden,
message-only Win32 window and supplies copied Skyrim key batches to the same
queue used by the renderer. It never opens or controls Skyrim.

```powershell
cmake -S tests/overlay-input -B out/build/input-tests -A x64
cmake --build out/build/input-tests --config Release
ctest --test-dir out/build/input-tests -C Release --output-on-failure
```

It also runs as `OverlayHotkeys` in the main compatibility suite. Coverage
includes either input route alone, matching in both orders across Present
drains, two presses in one interval, empty/consumed game batches, Windows
auto-repeat, non-removing peeks, downstream message consumption, focus loss,
modifier/extended scan codes, existing mouse bindings and text-editing policy.

The game listener registers through `BSInputDeviceManager` at InputLoaded,
with an idempotent DataLoaded retry. It forwards only initial keyboard button
presses, returns `kContinue`, and does not install a dispatcher hook. Menu state
and control/text-capture ownership still change only through the existing
Present handler.

Matching uses input-batch boundaries rather than a time-based debounce. A
Windows press can match the next Skyrim batch; an unmatched Skyrim press can
match Windows delivery before the next Skyrim batch. Present drains do not
erase those matches. Empty batches expire unmatched credits so a menu that
suppresses game input does not accumulate matches against future presses.
This assumes the two copies arrive in adjacent input intervals; arbitrary
reordering or a completely suppressed input dispatcher is not established by
the fixture. Actual ordering under Nolvus and other input mods needs game
validation before merge.

Pending game acceptance:

- Nolvus 1.5.97: keyboard toggle opens/closes the visible menu without Media
  Keys Fix; short taps and a held key do not produce duplicate toggles.
- LoreRim with KreatE: shared-key opening and closing still work.
- Close with End while editing a number, then verify capture release and
  movement. Numeric toggle bindings must not close while typing that number.
- Alt-Tab away/back must not apply a queued background press.

The earlier public PureDark implementation demonstrates the engine-event
approach in [SettingGUI.cpp](https://github.com/PureDark/Skyrim-Upscaler/blob/fa057bb088cf399e1112c1eaba714590c881e462/src/SettingGUI.cpp).
This change uses TRP's existing queued actions and capture policy; it does not
establish parity with any newer closed-source PD build.
