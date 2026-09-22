# Independent NR pass controls

Choose **Two** passes in **Neural Rendering**. The **Pass 1**
and **Pass 2** sections each contain input resolution, network preset and
**Intensity** and expandable **Appearance** controls. **Use Pass 1 settings** starts enabled; Pass 2 displays
the inherited values as read-only. Uncheck it to edit Pass 2 independently.
**Copy Pass 1** copies those controls without relinking them.
Relinking preserves your custom pass 2 values for later use.

Hover over an input-resolution slider to preview the edited model size. The left
column shows applied pass dimensions and network requests, inference timing, and
live FPS. It remains visible while scrolling the controls on the right.
Use Apply, Save as default or Discard for edits to either pass.

Both resolution percentages refer to the selected placement's scene size: the
render size before upscaling or output size after upscaling. Pass 2 processes
pass 1's result. Both retain separate model histories and remain on the same side
of upscaling. Placement, **Reconstruction**, peripheral compression, and
combined preparation stay shared. Returning to one pass hides Pass 2 without
erasing its custom settings.

When the model sizes differ, TRP resizes the first result in its prepared colour
space, evaluates the second model with its own guides, and transfers only that
second model's RGB change back onto the first result. An identity second pass
therefore preserves first-pass detail and alpha. This is intentionally different
from shrinking and then enlarging the whole final image. A larger second grid
cannot restore detail already absent from its input.

Changing resolution or preset retires GPU work before recreating the features
and may briefly pause the game. Tuning changes reset history without recreating
the features. Native UI is composed after both passes. Older NR runtimes cannot
use independent resolutions; that request is skipped before GPU work and can be
recovered by relinking. The existing final copy remains; copy elimination is a
separate optimization.

The existing NR inference timer includes inter-pass preparation and both model
evaluations. It excludes final pass-2 restoration/copy and final reconstruction.
Use the standalone benchmark's total timer for complete-pass cost comparisons.
No performance gain or game image-quality acceptance follows from offline tests.

Configuration is in `[SourceDLSSG]`: `NRPass2UseSameSettings`,
`NRPass2InputScale`, `NRPass2Preset`, `NRPass2Style`, `NRPass2Intensity`,
`NRPass2LocalTone`, `NRPass2LocalStructure`, `NRPass2SkinStructure`,
`NRPass2AutoSkinMask`, and `NRPass2UICorrection`. Missing overrides inherit the
first pass's saved values. The default link is true.

Validation: `NeuralPassSettings` checks real INI upgrades, independent round
trips, relinking, sanitization, and history/UI policy. The D3D12 debug-layer
fixture in `tests/nr-peripheral` checks separate creation/evaluation extents,
presets, tuning, identity detail preservation and nonidentity two-pass chaining,
UI composition, motion units, rounded sizes, and loading resets. The manual
vendor benchmark's `passes` mode exercises the actual runtime with both size
directions, placements, presets, and feature recreation; it is not run by builds.

For game acceptance: compare linked two-pass mode with unlinked pass 2 at 25%,
50%, and the first pass's resolution; vary only pass 2's intensity; relink; return
to one pass. Check camera motion, menus, and one loading transition. Save/reopen
the menu to confirm preferences. Test both placements if practical.
