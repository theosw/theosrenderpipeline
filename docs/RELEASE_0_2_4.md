# 0.2.4 release

This release combines the tested settings-menu revision, independent Neural
Rendering pass controls and opt-in NR keyboard shortcuts. NVIDIA runtime DLLs,
rendering defaults and existing ENB/Community Shaders/ReShade routes are retained.

## Changes

- Image, Neural Rendering, Frame generation and Advanced tabs keep related
  measurements beside controls, with FPS visible while settings scroll.
- The full-width pipeline bar shows applied rendering order and resolutions.
  Detailed runtime counters are hidden until Lab mode is enabled in Advanced.
- Window size, position and the draggable column divider are saved by Save as
  default. The frame-time graph grows with the window. Resizing is immediate;
  Apply and Discard continue to concern rendering edits.
- The second NR pass can use its own resolution, network preset and appearance
  settings. Linking and copying from Pass 1 are available. See the
  [NR pass guide](NR_PASS_CONTROLS.md) for controls and resolution-transfer rules.
- NR bracket shortcuts default off, including when an older INI lacks the new
  setting. The NR checkbox and End menu key remain available. Explicit
  `EnableNRHotkeys=true` restores the shortcuts.
- Saving defaults preserves configured runtime paths, including relative paths
  and intentional absolute overrides. Existing absolute paths are not guessed
  at or rewritten automatically.
- Startup verifies the selected route's game-code patches and preserves earlier
  import hooks. Repeated slot installation cannot capture TRP as its own original;
  unsupported second game-device creation fails before replacing the host.

## Validation and limits

The component builds passed Standard and Universal automated checks. The menu
and independent-pass candidates have scoped positive LoreRim ENB/RTX 4080 SUPER
feedback, with x4 and both NR placements covered across the recorded runs.
The final menu test has zero TRP error entries and four recurring NVIDIA RSYNC
flip-queue errors. Initial release preparation changed only version metadata and
documentation from that visually accepted menu checkpoint. The subsequent
[startup/settings corrections](RELEASE_0_2_4_HARDENING.md) add source changes;
their automated/static verification does not establish new game acceptance.

Saved-layout serialization and actual ImGui dragging pass the existing fixture.
Layout restoration after a game restart and individual physical-input sequences
remain separately unverified. New Standard gameplay, other hardware and CS runs
have not been performed for this menu revision. No performance gain or physical
frame-spacing improvement is claimed.

HDR remains unsupported and RTX 30 compatibility remains experimental. Temporal
reuse, bottleneck reuse and the standalone kernel experiments are excluded.
Existing runtime prerequisites, feature boundaries and attribution are retained.
Standard includes the eight NVIDIA runtimes; Universal supplies Ada/Ampere
compatibility and can use Standard's runtimes when installed after it in MO2.
