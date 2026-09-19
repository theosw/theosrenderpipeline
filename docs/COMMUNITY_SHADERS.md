# Community Shaders integration

Use CS for shading, upscaling, render scale, jitter, sharpening, tone mapping and
UI. TRP supplies NVIDIA frame generation, Reflex and optional Neural Rendering.
Disable CS frame generation and CS Reflex; leave CS upscaling enabled. No
replacement CS DLL is required. Disable other competing upscaler/FG plugins.

Set separate menu keys for CS and TRP/KreatE. F8 for CS and End for TRP works in
the tested setup; Bottle also offers Shift+F8 for its editor. Pick another key
if F8 is already used by FrameView or another tool. TRP does not rebind CS for you.

The tested CS setup uses SDR. CS HDR remains unverified and has an unresolved
report of an invisible TRP menu and inactive frame generation on CS 1.8.4.
Do not treat the SDR results as HDR support. CS combined with ReShade also
remains untested; use distinct menu keys if trying that combination.

## Rendering boundaries

The adapter preserves the engine postprocessing chain and snapshots motion,
depth and camera before CS rewrites them. Before-upscaling NR runs on CS producer
colour with a bounded working-range conversion and restoration to the producer's
original RGB range before CS continues. No gamma, gamut or exposure is inferred
from the texture format. After-upscaling NR runs on the
completed postprocessing scene, before UI. The final scene is captured for FG.
Both placements retain pass count, input scaling and reconstruction controls.

Ratio applies its maximum luminance gain after colour mixing; zero effect
strength restores the input exactly. Runtime capability checks reject unsupported
reconstruction requests before starting NR GPU work, leaving normal rendering
available. Supported settings can recover without a relaunch; real GPU failures
still use the existing failure and resource-retirement handling.

Where CS uses a separate UI compositor, the adapter replays its observed
single-output compute conversion into a private scene target without UI. It
accepts that conversion only after output is copied to the known game-facing
buffer. A missing handoff does not submit stale FG inputs. Camera history advances
at the actual host presentation boundary. Cross-API fences and resource retirement
remain responsible for shared NVIDIA resources.

Integration uses common engine locations, standard graphics fields and D3D
resource identity, with no private CS global offsets or shader hash allowlist.
This reduces fork dependence but does not establish compatibility with every CS
revision. Renderer ownership is selected at plugin post-load. The ENB path keeps
its own reconstruction and UI hooks.

## Editions and validation

Both Standard and Full compile the same NR/CS integration. Standard uses native
NVIDIA FG and includes the runtimes; Full adds Ada/Ampere compatibility and can
use Standard's runtimes when installed after it in MO2. NR defaults off.

Earlier Full checkpoints were tested on Skyrim 1.6.1170, RTX 4080 SUPER and
Bottle's CS 1.8 build with Effects 11. Both NR placements at one pass/100% input
were visually accepted. A later Ratio test confirmed that lowering the maximum
luma ratio darkens the scene and zero effect restores its colour. Separate CS
and TRP menu keys were also accepted. These observations do not accept other CS
builds, HDR appearance, native RTX 50 operation or physical frame cadence.
The combined 0.1.4 Standard and Full builds have overall positive
reports on that setup, with logged native x2 in Standard, Ada x4 in Full, both
NR placements and later Full x6. Full recorded zero NR/host failure counters
alongside two recurring Streamline RSYNC errors. These reports do not separately
verify every UI/transition check. Both editions also received positive Cabbage
ENB regression reports with native x2 in Standard, Ada x4 in Full, and both NR
placements. Full's final ENB log has no renderer error entries or NR/host failure
counters; Standard ENB retains two recurring RSYNC entries. ENB/KiLoader loaded
and generated the ENB cache on the first launch without an extra restart.

CS's "D3D12 Swap Chain: Inactive" footer describes its own disabled frame-generation
proxy. TRP owns a separate Streamline swapchain, so this status is expected here.
Keep CS frame generation disabled.

Offline fixtures cover resource/guide isolation, binding restoration, display
conversion without UI, presentation confirmation, frame consumption and resize.
NR contract tests cover placement, history, settings and runtime preflight;
production-HLSL WARP tests exercise Ratio reconstruction. Those fixtures do not
execute the installed CS engine hooks or prove generated-frame presentation.

For a focused candidate test, compare NR off with Before and After upscaling at
one pass/100% input in the same bright scene. Check HUD/menu colour and one cell
transition. Test Standard native x2 first, then Full x4 with the same runtimes.
