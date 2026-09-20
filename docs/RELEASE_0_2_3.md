# 0.2.3 release candidate

This candidate combines the Nolvus keyboard-input fix with two optional Neural
Rendering optimizations. Existing DLAA, Community Shaders, ReShade, NR tuning
and Ada/Ampere frame-generation support are retained.

- **Peripheral compression** reduces the NR pixel workload toward the image
  edges while preserving more detail around the centre. It can change edge
  quality, and composes with the existing NR input-resolution setting.
- **Combined preparation** combines eligible colour and guide preparation
  passes while preserving their filtering and format rules. Benefits depend
  on the active route; a gameplay performance improvement is not established.
- **Nolvus keyboard input** uses Skyrim key events when Windows messages do
  not reach TRP, while retaining the Windows route for menus such as KreatE.
  See the [Nolvus setup guide](NOLVUS.md) for tested configurations and limits.

Both NR options default to off and have independent controls. Apply changes
in the Neural Rendering menu; Save as default makes them persistent. Temporal
reuse is excluded after a test reported microstutters. NR still evaluates on
every source frame when active.

## Validation status

The component candidates have offline test coverage and scoped positive user
feedback. Nolvus F10 input, x5, both NR placements and Wheeler worked in the
recorded Universal test. LoreRim/KreatE shared End also passed its requested
regression. Peripheral compression and combined preparation were accepted in
an earlier ENB experiment; its temporal-reuse path is not included here.

The combined source passes fresh Standard and Universal Release builds, all
51 registered CTests and 30 offline SKSE Query/version cases per edition.
The production GPU tests cover both NR options together, native/CS contracts,
both placements, one/two passes, format rounding, loading resets and fresh UI.
Merged PR32 and PR34 produce the exact source tree used by those checks.

The integrated Universal candidate at `72c026d` has positive overall LoreRim
ENB feedback. Its log records x4, both NR placements, separate/together use of
the new options and recovery after loading. The user reported corrupted
percentage text in the peripheral description; that formatting call and the
DLAA percentage tooltip are corrected. Visual verification of the text fix
remains pending. This run does not establish Standard gameplay, other hardware,
a performance gain or smooth physical frame spacing.
Retained game logs include recurring Streamline RSYNC errors; the early Nolvus
OAR/IED loading-panel limitation remains. The separate DLSS-G VRAM-budget
warning correction in PR31 is not included in this integration.
