# Settings action recovery

Rejected Apply and Save as default operations now display their error ahead of
pending-change counts and log the action and reason. The draft remains editable.
Long errors reserve footer space. Success logs distinguish requested mode/quality
from the currently running allocation, which can differ until restart.

An inactive Dynamic target no longer blocks a fixed multiplier. Loading or saving
an invalid target resets that field to display refresh without discarding the
selected multiplier or Dynamic choice. Valid remembered targets are retained.

If dedicated UI composition or the NR runtime becomes unavailable, turning NR off
remains possible. Unchanged NR requests can accompany unrelated settings changes;
execution stays gated by the runtime and composition capabilities. Changing or
newly enabling unavailable NR is rejected. The panel points to the actual
NativeUICompositionMode startup setting instead of the independent Native UI
checkbox. Saving preserves explicit edits to that startup setting on disk,
including migration of the legacy key. INI comments use the current edition and
quality names.

RendererSettingsActions exercises error/log propagation, pending-status precedence,
generation load/save cases and NR capability loss. StartupPreferences covers
composition-key preservation/migration; NeuralWorldContract retains the native
and external-world capability matrix. These three focused Release checks pass.
They do not reproduce the original remote DLAA report or replace an in-game
combined-edit Save/restart check. Menu appearance and game persistence remain
acceptance items for the combined candidate.
