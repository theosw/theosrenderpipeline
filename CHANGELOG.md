# 0.2.4

- Reorganized settings into Image, Neural Rendering, Frame generation and
  Advanced, with live FPS and related measurements beside their controls.
- Added a resizable window and draggable column divider. Save as default now
  remembers the window position, size and divider; the frame-time graph scales
  with the available space.
- Added independent input resolution, network preset and image tuning for the
  second NR pass, with linking and copying from Pass 1.
- Added descriptive DLSS preset labels and moved detailed runtime information
  behind Lab mode in Advanced. The live pipeline overview remains above the tabs.
- Disabled the NR bracket shortcuts by default to avoid shared-key conflicts.
  Set `EnableNRHotkeys=true` under `[Hotkeys]` to restore them; the NR menu
  checkbox remains available. Shortcut requests now appear in the log.
- Keep configured runtime paths portable when saving default settings.
- Preserve earlier renderer hook chains and check game-code patches before
  installation, with clear errors for incompatible or repeated device setup.

Existing NVIDIA runtime DLLs are unchanged. HDR remains unsupported and RTX 30
compatibility remains experimental. Temporal and bottleneck reuse are excluded.

# 0.2.3

- Fixed settings-menu keyboard input on tested Nolvus setups while retaining
  compatibility with LoreRim/KreatE.
- Added optional NR peripheral compression to reduce model pixel workload
  toward the screen edges.
- Added optional combined NR preparation passes. Both optimizations default off.
- Fixed corrupted percentage text in the NR description and DLAA tooltip.
- Fixed a renderer shutdown when NVIDIA reports a VRAM-budget warning after
  accepting frame-generation settings. Memory pressure and related hitches may
  still occur.

Temporal reuse is excluded. HDR remains unsupported; RTX 30 compatibility
remains experimental. Existing NVIDIA runtimes and default settings are retained,
with the two new NR options disabled by default.

# 0.2.2

- Enabled Neural Rendering with DLAA, before or after anti-aliasing.
- Improved experimental RTX 30-series compatibility in Universal, tested with
  Community Shaders on an RTX 3060 Laptop.
- Fixed the compatibility default when upgrading older configurations with a
  missing key, while respecting explicit opt-outs.
- Improved startup diagnostics for renderer edition, settings, GPU and
  initialization failures.

RTX 30 requires Universal; Standard can supply its NVIDIA runtimes. RTX 30
support remains experimental, with artifacting and intermittent hitches reported.
HDR remains unsupported. Existing features, packaged defaults and runtimes are
retained; the NR optimization experiments are not included.
