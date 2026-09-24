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
the fixture. Game evidence is limited to the recorded configurations below.

Nolvus candidate `f078b1b` received positive user feedback for the requested
F10 taps/holds, defocus-before-closing, movement and Alt-Tab checks on Skyrim
1.5.97. The same input source, in local LoreRim integration `f37be730`, received
positive feedback for shared End with KreatE, active-edit closure/movement and
Alt-Tab recovery. Its log records six End open/close pairs and two releases of
active text capture at closure. The exact input device was not separately
identified.

These are scoped input passes. Standard gameplay and the final release
integration remain untested; arbitrary delayed delivery and complete dispatcher
suppression remain outside the established matching contract. Numeric-binding
protection is covered offline, not by these End/F10 game tests.

The earlier public PureDark implementation demonstrates the engine-event
approach in [SettingGUI.cpp](https://github.com/PureDark/Skyrim-Upscaler/blob/fa057bb088cf399e1112c1eaba714590c881e462/src/SettingGUI.cpp).
This change uses TRP's existing queued actions and capture policy; it does not
establish parity with any newer closed-source PD build.

Edition validation of renderer source `f078b1b`: Standard and Universal Release
builds pass, with all 50 compatibility CTests passing per edition. Both DLLs
also pass 30 offline SKSE Query/version cases each; those checks do not call
SKSEPlugin_Load or validate gameplay. Standard retains NR and uses native NVIDIA
frame-generation capabilities; Universal retains Ada/Ampere compatibility.
