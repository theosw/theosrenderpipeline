# 0.2.4 startup and settings corrections

This change addresses the release review against `ea085ba`.

- Save as default retains configured runtime path spellings. Startup uses
  separately resolved paths; intentional absolute paths and settings edited on
  disk during the session remain intact.
- The GetClientRect import hook preserves the prior target. D3D11 imports and
  device/context/swapchain slots use the same guarded installation mechanism.
- Before installing renderer game-code patches, validate the selected native
  or Community Shaders route. Direct calls must retain their expected engine
  callee or an executable external hook target; GetClientRect must use its named
  import slot. NOP ranges require their exact verified original instructions.
  Existing supported entry/call detours are preserved.
- Offsets in the reviewed renderer installation path now come from the exact runtime profile,
  including DRS, InitD3D, native dimensions and the CS boundary.
- Reinstalling a known slot preserves its original target and later hooks.
  Conflicting forwarding targets fail explicitly. A second/reentrant game-device
  creation is refused before it can replace the host's active resources.

The public fixtures cover load/resolve/save/reload after moving the game root,
intentional absolute paths, on-disk edits, a real executable IAT chain,
repeated/concurrent installation, conflicting vtables, changed/unreadable code,
and exclusive game-device admission. Private executable evidence verifies 16
contracts for each of the four admitted runtimes. It is static verification,
not game acceptance on additional Skyrim versions.

Both editions build and each passes 52 CTests and 30 offline SKSE Query cases.
Two hardware fixtures (ReShadeAbsent and SourceDLSSGInteropFence) and the longer
NeuralPeripheralPixels software fixture were deferred while another Skyrim run
was active. New game startup with ENB/CS, live mod chains, and physical cadence
remain unverified. These checks do not invoke SKSEPlugin_Load or replace a
game test. Build identities are retained in the development checkout's report.
No deployment, release ZIP replacement or game launch is implied.
Missing private developer headers, UI-blend suite integration (PR #26), DLSS
preload search policy and tracing teardown remain separate follow-ups.
