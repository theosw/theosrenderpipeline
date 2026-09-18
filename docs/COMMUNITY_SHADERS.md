# Community Shaders adapter prototype

The initial prototype received scoped x4 gameplay confirmation, but NR caused
colored sky artifacts in both placements. This candidate changes the later NR
boundary; its visual result is untested. Validation targets Skyrim 1.6.1170
with Community Shaders in the UltraCS profile. Support for other
CS revisions, lighting modes and Skyrim versions is not established by building
the plugin or by the existing runtime-profile tests.

CS owns shading, reconstruction, render scale, jitter, sharpening, tone mapping
and UI. TRP retains its NVIDIA frame-generation host, multiplier/latency controls
and optional NR. Disable CS frame generation and CS Reflex before the combined
run; leave CS upscaling enabled. No replacement CS DLL is required.

The adapter wraps the engine postprocessing call while preserving the earlier
renderer hook. It snapshots motion, depth and camera before CS rewrites them.
Before-upscale NR still runs there on unfinished world color. After-upscale NR
now waits until the preserved engine postprocessing callee returns and operates
on the actual framebuffer RTV's scene, before CS restores any framebuffer
redirection and before UI rendering. The corrected scene is then captured for
FG. Previously both placements evaluated before tone mapping. The early path
remains unvalidated and is not part of this correction. Both retain passes,
input scaling and reconstruction settings. A float texture alone does not
determine the correct HDR reconstruction setting; HDR appearance remains untested.

The completed scene is captured before UI. Where CS uses a separate UI
compositor, the adapter replays its observed single-output compute dispatch
into a private scene target without UI. It accepts that conversion only after
the output is copied into the known game-facing buffer. An unavailable handoff
leaves normal scene presentation working without submitting stale FG inputs.
Camera history advances at the host's actual presentation boundary, below any
CS suppressed-Present traversal. Existing cross-API fences and resource
retirement remain responsible for shared NVIDIA resources.

This implementation uses common engine locations, standard graphics fields and
D3D resource identity. It has no private CS global offsets or shader hash
allowlist. This reduces dependence on a particular fork but does not prove
compatibility with every CS version. Renderer ownership is selected at plugin
post-load; the ENB path retains its own reconstruction and UI hooks.

Offline WARP tests cover guide/scene overwrite isolation, active extents,
graphics/compute binding restoration, display conversion without UI, additional
output rejection, presentation confirmation, frame consumption and resize.
NR contract tests cover world-only placements, history resets and settings
validation. They do not execute CS's engine hooks, NR inference or generated
frame presentation.

For the next test, use the same bright sky with NR off, then after-upscaling NR
at one pass and Auto/100%, leaving x4 unchanged. Confirm the logged input is the
post-processing scene, check that UI stays untouched, and verify one transition.
Preserve the CS-only baseline and record the exact package and live settings.
Do not infer visual acceptance from Present counts or offline test results.
