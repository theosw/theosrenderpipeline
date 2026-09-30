# Unreleased

- Reduce HUD ghosting in generated frames: DLSS-G now interpolates the HUD-less
  scene and the UI separately (UI recomposition). On by default, including for
  existing INIs; toggle it live under Frame generation. Small GPU/VRAM cost.

# 0.3.4

- Added experimental TRP HDR output for ENB and other non-CS setups: expand the
  finished SDR image to HDR10 with paper white, peak, UI brightness and highlight
  controls. Paper white and UI follow Windows SDR content brightness by default.
  Off by default; enabling it needs Save as default and a restart.
- Keep late overlays visible over opaque UI in HDR, with matching frame-generation
  UI/HUD-less layers and a separate UI brightness.
- Fixed GPU device loss with Modex 3D item previews under DLSS by routing their
  copies and clears to the native UI target while Modex draws.
- Drop and log copies that fall outside the game-facing/native UI textures, and
  include D3D11/D3D12 removal reasons in device-loss diagnostics.
- Fade forced loading artwork in from black on door transitions, avoiding a brief
  bright flash.

Community Shaders HDR Display integration is retained. HDR remains experimental;
ENB acceptance covers Universal on one RTX 4080 SUPER with x4, early NR, Wheeler,
HUD/End over menus and loading recovery. Standard HDR, other hardware and physical
frame cadence remain unverified. Recurring NVIDIA flip-queue errors are unresolved.
Both editions retain NR and existing compatibility routes. NVIDIA runtimes are
unchanged; TRP HDR output defaults off. See the README HDR notes for setup.

# 0.3.3

- Added experimental HDR with Community Shaders HDR Display. TRP identifies the
  UI before CS composes HDR, so the menu stays visible and frame generation runs.
- Frame generation receives CS's own HDR10 conversion of the HUD-less scene.
- NR after upscaling keeps HDR highlights and its normal look on CS's HDR scene.
- D3D11 capture hooks now survive runtime state changes.
- ReShade screenshots skip already-encoded HDR output instead of miscolouring it.
- Fixed camera capture dereferencing stale cached camera pointers by selecting
  and retaining a live camera.
- Fixed the optional direct DLSS and direct sharpening output routes on eligible
  textures, with allocation fallback and clearer failure diagnostics. Both
  options remain off by default.

HDR requires CS HDR Display; ENB-style weathers also need CS Effects 11 with a
preset. See the README's HDR notes for setup and tested scope.
NVIDIA runtime DLLs and configuration defaults are unchanged. Both editions
retain NR and their existing frame-generation capabilities.

# 0.3.2

- Added weather and time presets for individual NR settings and sharpening,
  with Base inheritance, interior/weather assignments and per-setting reset.
- Added shareable preset INI files, list priority, and full preset copies.
- Moved presets and sharpening into the Neural Rendering tab alongside the
  Pass 1 / Pass 2 controls; added keyboard-layout-aware text entry.
- Added optional one-pass NR during combat or while weapons/spells are drawn,
  with a configurable delay before the second pass resumes.
- Corrected ReShade screenshots using a subsequent completed frame containing
  DLSS, NR and the HUD; the existing capture limitations remain documented.

Both editions retain NR and their existing frame-generation capabilities.
NVIDIA runtime files are unchanged. HDR remains unsupported. Preset files use
the new format and are not readable by earlier builds; preserve settings before
switching back to an older version.

# 0.3.1

- Allow frame generation and Neural Rendering during RaceMenu character creation,
  while retaining the loading-screen and fade guards.
- Reduce NR activation work by embedding its shaders at build time and reusing
  the verified runtime identity when toggling a retained NR feature. First-time
  NVIDIA model and pipeline creation can still cause a pause.
- Support Bottled Shaders' reverse-Z depth format and forward the correct depth
  convention to frame generation and both NR placements.
- Correct GPU timing scopes and the labels for rendering-stage measurements.

NVIDIA runtime DLLs and configuration defaults are unchanged. HDR remains
unsupported, and RTX 20/30 compatibility retains the documented test limits.

# 0.3.0

- Added experimental RTX 20 frame-generation compatibility in Universal.
- Select compatible PTX endpoint networks on Turing instead of incompatible
  precompiled kernels, with runtime fingerprints and preparation checks.
- Stop clearly at the first real RTX 20 kernel-loading failure and identify the
  rejected program, preserving the runtime's API-presence probes.
- Retained all released 0.2.5 startup, settings and runtime-diagnostic fixes.

The RTX 2060 compatibility test has positive x2/x3/x4/x6, NR and loading-recovery
evidence. The combined release has positive ENB/RTX 4080 SUPER regression feedback
with x4/NR and loading recovery. RTX 20 support remains experimental; see
[compatibility limits](README.md#compatibility). NVIDIA runtimes and configuration
defaults are unchanged. HDR remains unsupported.

# 0.2.5

- Fixed startup compatibility with SSE Display Tweaks BorderlessUpscale and
  existing renderer hooks, including Community Shaders postprocessing.
- Save and Apply now show and log why a settings change was rejected.
- Fixed hidden Dynamic frame-generation settings blocking unrelated edits or
  resetting the selected multiplier.
- Kept NR's off control available when its requirements are unavailable, and
  allowed unchanged NR preferences to survive unrelated settings saves.
- Preserved explicit startup composition settings when saving other options,
  and clarified requested versus active upscaling settings in the log.
- Added clearer startup diagnostics when an observed NVIDIA App FG override
  prevents the configured frame-generation runtime from loading.

NVIDIA runtime DLLs and packaged defaults are unchanged. HDR remains unsupported
and RTX 30 compatibility remains experimental. The original remote DLAA reset
has not been reproduced; these changes are not claimed to establish its cause.

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
