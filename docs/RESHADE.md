# Experimental ReShade integration

TRP can run ReShade effects once per source frame, before or after upscaling,
with an explicit scene-depth input. Effects complete before the scene is handed
to frame generation. ReShade's overlay is drawn at the native UI boundary.

This feature has standalone validation with ReShade **6.3.3.1921 / add-on API
14**. Skyrim, ENB/CS preset appearance, physical input, MFG cadence and performance
remain unverified for this candidate. Other ReShade versions and custom forks
have not been validated.

## Settings and installation

Use an ordinary ReShade installation and its existing `ReShade.ini`, preset and
shader paths. TRP does not distribute ReShade or rewrite its bindings. The
configured effects toggle and overlay key continue to belong to ReShade.
`[GENERAL] Disable` is respected.

Leave **SSE ReShade Helper disabled** for this candidate: TRP supplies its own
effect and UI stages, and the helper-enabled configuration previously lost the
device. Compatibility with the helper has not been established.

The Advanced panel offers **ReShade before upscaling**. The equivalent saved
setting in `TheosRenderPipeline.ini` is:

```ini
[Compatibility]
ReShadeBeforeUpscaling=false
```

After upscaling is the default. Before upscaling uses the active render extent;
after upscaling uses the output extent. A change can reload ReShade shaders. The
first result after a target extent/format change is discarded while the runtime
adapts, avoiding an old-size effect pass appearing on the new target.

Before-upscaling presets must tolerate the producer's color representation.
In particular, CS supplies unfinished HDR scene color at that boundary. The
default after-upscaling stage receives the completed scene. Do not assume a
preset has identical appearance in both positions or that switching position
guarantees a particular speedup.

## Frame ordering

Only the selected ReShade position runs:

| Producer | Order |
| --- | --- |
| TRP upscaling / ENB | Optional early NR → early ReShade → DLSS or loading-image scaling → late ReShade → existing preparation / optional late NR → native UI |
| Community Shaders | Optional early NR → early ReShade → CS upscaling and engine post-processing → optional late NR → late ReShade → scene snapshot → native UI |

The existing NR placements, passes, tuning and MFG selection are preserved.
Both scene capture routes include the selected effects before FG consumes the
scene. The output presentation chain has no independent ReShade effect runtime.

TRP crops the active depth rectangle into an R32_FLOAT texture. When effects
run at output resolution, it point-scales depth to that extent, preserving
foreground/background discontinuities. The published `DEPTH` is raw Skyrim
reversed-Z depth; presets remain responsible for their depth interpretation.
Menu/loading or missing-guide frames get explicit zero/far depth rather than
the previous world's depth.

World/menu evaluation and Present fallback share the same per-source-frame
guard. If no scene boundary runs, UI updates still process ReShade input and
draw its overlay without applying effects to the HUD. ReShade overlay state
participates in TRP's paired game-control suppression. Physical close/movement
acceptance must still be checked in Skyrim.

## Ownership and lifetime

TRP resolves the public API from an already loaded ReShade module. It registers
before creating its own D3D12 output device and uses the public `get_native()`
handle from the scoped device-init callback for that device and its queues.
This prevents an automatic ReShade runtime on the generated-output swapchain.
Unrelated D3D12 device creation and the game-facing D3D11 interfaces are unchanged.

One explicitly owned D3D11 runtime processes effects, shader reloads, input and
the GUI. Its offscreen swapchain facade never presents to DXGI. Keeping a second
automatic runtime merely with effects disabled is insufficient: ReShade runtimes
share window input, and output Presents can consume pending hotkeys before the
source runtime processes them.

The integration restores the D3D11 context around its work. Private color/depth
resources never become FG tags. Runtime and host resources are released at the
existing retirement boundary; native UI format/size replacement first waits
for the owned D3D11 runtime. No private ReShade GUID, vtable offset or binary patch
is used. If public registration or native-output capture is unavailable, the
source stages stay inactive and status reports the automatic fallback.

## Standalone verification

With compatibility tests enabled, CTest includes `ReShadeAbsent`, which verifies
that no-injector operation is inert. `SourceNvidiaFrameEvaluation` checks stage
ordering across 8,192 native and 256 supplied-frame combinations.

For an actual-runtime check, create an ignored directory under `out/`, copy
`TRPReShadeIntegrationTests.exe` there from the build, and copy the contents of
`tests/fixtures/reshade/` beside it. Supply your own 64-bit ReShade 6.3.3 DLL as
`dxgi.dll`. Run from that directory:

```powershell
./TRPReShadeIntegrationTests.exe --require-reshade
```

The fixture creates its own hidden window and D3D11/D3D12 devices. It does not
launch Skyrim. It checks effect pixels and depth extents in both positions,
live placement changes, four actual output Presents per source frame without
duplicate effects or lost F8/Home input, effects-off behavior, zero menu depth,
context restoration and runtime recreation after retirement. Synthetic input
is posted only to that fixture window. Restore the fixture INI between runs,
since ReShade saves its runtime configuration.

These checks do not load Streamline or run NR/MFG inference. A future game test
must verify the intended build, both ReShade placements, NR off/both positions,
overlay close and game controls, loading/menu transitions, and x2/x4 operation
before drawing performance or release-acceptance conclusions.

## Existing device-identity regressions

The preceding compatibility fixes remain included. ReShade can expose a proxy
device while child fences or cached compute shaders report the underlying
device. TRP accepts its own fence's native owner and retains the actual creation
device for cached depth shaders, while continuing to reject foreign devices.
Queue waits, partial-startup handling and retirement checks remain intact.

The production-interop fixture checks same-device fences at zero/nonzero values,
foreign WARP fences, pending input waits and completion. The depth-copy fixture
checks repeated captures, three formats, cropping, cache reuse, restored state
and foreign contexts/resources. For either wrapped regression, copy the executable
into a separate ignored directory beside the identified ReShade `dxgi.dll` and run:

```powershell
./TRPSourceDLSSGInteropFenceTests.exe --require-wrapped
./TRPD3D11FrameCopyTests.exe --require-wrapped
```

The flag requires the observed proxy/native identity split; it cannot pass by
silently omitting the injector. Normal CTest runs the unwrapped comparisons.
Retain the DLL version/hash and ReShade log with results. These fixtures do not
create a swapchain or run Skyrim. The new integration fixture above separately
exercises real presentation and effects. Effects-off leaves ReShade hooks loaded
and must not be treated as a DLL-absent comparison.
