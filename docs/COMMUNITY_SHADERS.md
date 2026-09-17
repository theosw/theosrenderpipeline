# Community Shaders adapter prototype

This branch has no combined Skyrim acceptance. Initial validation targets
Skyrim 1.6.1170 with Community Shaders in the UltraCS profile. Support for other
CS revisions, lighting modes and Skyrim versions is not established by building
the plugin or by the existing runtime-profile tests.

CS owns shading, reconstruction, render scale, jitter, sharpening, tone mapping
and UI. TRP retains its NVIDIA frame-generation host, multiplier/latency controls
and optional NR. Disable CS frame generation and CS Reflex before the combined
run; leave CS upscaling enabled. No replacement CS DLL is required.

The adapter wraps the engine postprocessing call while preserving the earlier
renderer hook. It snapshots motion, depth and camera before CS rewrites them.
Before-upscale NR runs there; after-upscale NR runs at the preserved engine
callee, after CS reconstruction and before engine postprocessing. Both stages
exclude UI and retain passes, input scaling and reconstruction settings. A float
texture alone does not determine the correct HDR reconstruction setting.

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

For the first combined game test, verify startup and both menus, x4 MFG with NR
off, NR before and after CS upscaling, one/two passes, End closure while editing,
F12/Wheeler alignment, and one cell transition. Preserve the CS-only baseline and
record the exact package and live settings. Do not infer visual acceptance from
Present counts or offline test results.
