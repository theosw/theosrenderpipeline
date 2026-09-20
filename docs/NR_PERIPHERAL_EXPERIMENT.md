# Experimental peripheral NR compression

This branch adds an opt-in spatial experiment on top of 0.2.2. It is not part
of the 0.2.2 release archives. `NRPeripheralCompression=false` in `[SourceDLSSG]`
retains the existing uniform path. The NR panel exposes the same checkbox.

The symmetric 80/90 layout keeps the central 80% of each axis at the selected
NR input sampling density, compresses the edges, and uses 90% of each work
dimension (approximately 81% of the model pixels). Existing NR input scale
applies in addition. At 100% input and 5120x1440, the model processes 4608x1296.
This is a pixel budget, not an FPS guarantee or pixel-identical centre output:
the model sees different context, and edge quality can change.

## Integration

- The shared NR pass owns packing and reconstruction in both native and CS
  placements. Colour and optional model UI use the existing area filter over
  inverse-mapped footprints. Depth and motion use matching nearest positions.
- Motion is converted from the producer's guide units to scene pixels, then
  transformed as `Pack(current + motion) - Pack(current)`. Offscreen endpoints
  use the curve's linear continuation; they are not clipped to the viewport.
- Packed guides match the model extent and carry pixel displacements with unit
  NGX motion scale. The producer extent remains the resource-recreation key.
- Residual/Ratio reconstruction samples the packed coordinates but applies NR
  changes to the original full-resolution scene. CS producer-RGB normalization,
  signed channels, alpha and native UI composition remain separate.
- Each selected NR pass still evaluates once per source frame, with distinct
  feature histories for two passes. Changing layout triggers the existing
  drain/recreation/reset path. No queue, retirement or failure policy is changed.
- Legacy NR runtimes that lack reconstruction skip this optional mode through
  the existing preflight check. Disabling it restores their old direct path.

The radius curve comes from BeliyG3/optimizer-fps-dlss5 at
`71f5cfa19e806008c59d770bcff86513c1df3760`; its MIT notice and precise scope are
in THIRD-PARTY.md. No reference hooks, temporal reuse or async scheduler are used.

## Offline evidence, September 20, 2026

`NeuralPeripheralPixels` executes the production D3D12 kernels, NR pass and
transport on WARP with validation enabled. Its NGX boundary is a scripted
identity model, so this establishes host mapping/composition/state behavior,
not learned image quality. It checks mixed guide sizes, odd rounded extents,
offscreen endpoint motion, centre density, depth sampling, nonzero residual
reconstruction, signed producer identity, alpha, native UI, real/FG colour
agreement, repeated retired slots and separate one/two-pass histories.
The world/configuration fixtures cover defaults, INI persistence, legacy
admission and layout-triggered history/resource changes.

An opt-in `TRPNeuralPeripheralBenchmark` uses the production FeatureSession and
locally supplied, pinned Nexus NR 310.8 runtime (`8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206`). No vendor
DLL is distributed with these fixtures and CTest never launches the benchmark.
The local RTX 4080 SUPER comparison uses a static synthetic FP16 scene at
5120x1440, one pass, default tuning, full-sized original guides, UI correction
off, 10 warmup frames and 40 measured frames per process. D3D12 validation is
off for hardware timing. Both clocks are fence-retired GPU elapsed spans.

| Mode | Model size | First order: whole NR / inference (ms) | Reverse order: whole NR / inference (ms) |
|---|---|---|---|
| Native | 5120x1440 | 13.982 / 13.978 | 13.890 / 13.886 |
| Uniform 90% | 4608x1296 | 13.493 / 12.969 | 12.044 / 11.556 |
| Peripheral 80/90 | 4608x1296 | 12.207 / 11.527 | 12.160 / 11.475 |

"Whole NR" includes preparation, inference and reconstruction, but excludes
the original input copy, remaining game rendering, presentation and FG. The
uniform first run varied substantially; these short runs do not establish a
peripheral speed advantage over uniform scaling. The repeat's approximately
1.73 ms saving against native is an NR-stage observation, not a Skyrim FPS gain.
Packed guide dimensions also differ, so these compare entire paths rather than
isolating the spatial curve. All measured outputs were finite and changed by NR.

**The hardware benchmark does not exit cleanly.** Both native controls and all
spatial runs release the NR feature after fence-proven completion, then stall
in the standalone normal NGX shutdown call. Removing debug-layer overhead did
not fix it. The runner terminates only its own timed-out child. Preserve those
failed exit results alongside the collected timings; they are not successful
lifecycle tests, and the cause remains unproven. Renderer shutdown was not
changed to accommodate the fixture. The first fixture also omitted the normal
NGX bootstrap; correcting that harness precondition enabled the actual tests.

Raw logs, samples, timeout records and local binaries are under this checkout's
ignored `out/diagnostics` and `out/build/peripheral-fixtures` directories.

## Build and game evidence

Both Standard and Universal build successfully and each passes 50 registered
CTests and 30 offline SKSE Query cases. The production-pass fixture covers 32
configurations across native/CS, early/late placement, one/two passes and
full/half input scale, with compression both off and on.

The first ENB game comparison used RTX 4080 SUPER, 5120x1440 output, DLSS Quality
with a 3413x960 scene, x2 frame generation and one NR pass before upscaling.
The user reported difficulty distinguishing the modes and accepted the result.
Retired early-NR GPU round-trip samples gave:

| NR input / compression | Model extent | Round trip (ms) |
|---|---|---|
| 100% / off, two intervals | 3413x960 | 6.645 / 6.789 |
| 100% / on | 3072x864 | 6.068 |
| 90% / off | 3072x864 | 6.048 |
| 90% / on | 2765x778 | 5.539 |

These spans include copies and cross-API scheduling. Cumulative sample deltas
exclude the first five seconds after each mode change and pairs more than eight
seconds apart. Observation lengths differ and no matched camera capture was
retained. Compression saves roughly 0.6-0.7 ms against full input in this run;
it does not establish an advantage over uniform 90% or a whole-game FPS gain.
The final setting combines both reductions. There were no logged NR failures,
but one NVIDIA RSYNC flip-queue error occurred, followed by continued rendering.

CS gameplay, after-upscaling gameplay, MFG, RTX 30 performance, detailed moving
edge/centre quality, individual UI/transition cases and physical cadence remain
unverified for this candidate. Keep the feature opt-in while those are assessed.

Standalone fixture build: `cmake -S tests/nr-peripheral -B out/build/peripheral-fixtures`,
then build Release and run CTest for that directory. The optional hardware
target requires `TRP_NR_VENDOR_BENCHMARK=ON` and a local `NGX_SDK`; its usage is
`TRPNeuralPeripheralBenchmark <absolute-NR-DLL> native|uniform|peripheral <width> <height>`.
Run it in a bounded child process because of the recorded shutdown limitation.
