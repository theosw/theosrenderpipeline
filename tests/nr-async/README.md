# Standalone asynchronous NVIDIA NR prototype

This experiment is based on the asynchronous capture/evaluate/compose idea in
[RedDukeDev's design](https://github.com/RedDukeDev/dlss5-nr-amd-custom/blob/058e8a998cfba5643081ac5fb29eecc2ad3bd2b5/docs/DESIGN.md).
It uses TRP's existing native NVIDIA feature session and runtime checks. It does
not use ZLUDA, AMD kernels, OptiScaler, or copied upstream source.

The experimental pipeline now lives under `src/FrameGen` and is also used by
the renderer's opt-in **Execution: Async** control. Regular NR remains the
default (`NRAsync=false`). Building this standalone harness does not deploy or
launch a game. Async is disabled by default in `AsyncPipeline::Config`.

The game owner uses immutable per-capture options, native RG16/RG32 guides at
their own extent, one or two independently configured passes, and a live adapter
budget with a 2 GiB history cap and model/rendering reserve. Unsupported formats,
insufficient budget and late native UI composition retain regular NR with an
explicit status. Async currently needs a world-only scene; CS provides both
placements, while the native route provides its before-upscale scene. The host
skips busy capture admissions and invalidates the missing motion chain.

`TRPNeuralIntegrationBenchmark` is a manual NVIDIA probe of the production
execution wrapper: Regular / Async / independent two-pass Async / Regular,
different guide/scene extents, a scene resize with unchanged guides, finite
outputs, alpha, retirement and shutdown.
It requires Skyrim closed and does not establish game or MFG acceptance.

## Ownership and scheduling

- The host records snapshot copies, motion tracking and composition on its queue.
  The host must supply actual COMMON-state textures and a completion fence for
  every submitted frame. Only one host frame may be outstanding. `Record` polls
  admission and returns `S_FALSE` if that frame has not retired; it never waits.
- A worker owns a separate direct queue, allocator, command list and native NR
  feature. It waits for snapshot capture completion, records inference, and marks
  output ready only after its evaluation fence completes. Shader descriptors and
  allocators are reused only after that completion. There are three private
  snapshot slots: displayed result, active evaluation and latest pending capture.
- The host replaces a pending capture with the newest retired frame. It carries
  each outstanding snapshot's motion map forward even while inference is running.
  Per-link depth rejection makes a broken history chain stay invalid. A ready
  result never causes a host-queue wait on inference.
- Composition samples the difference between captured and enhanced colour through
  that motion map and applies it to the current scene. Offscreen, invalid-motion,
  depth, changed-colour, old-generation and expired-result cases retain the
  current scene. Gain limits are reapplied against current colour; signed producer
  channels and alpha are preserved. There is no disocclusion filling.
- Reset and disable discard visible/pending results immediately. An evaluation
  already in progress keeps owning its inputs; its old-generation answer is
  discarded when it completes. `Stop` joins the worker and checks both GPU owners.
  Unconfirmed submission/retirement keeps the entire state and evaluator alive for
  process lifetime. A failed native recording is terminal and is never submitted.

The asynchronous prototype deliberately resets the model's own temporal history
on every evaluation. The original `VendorBenchmark` resets both native modes.
The paced replay below adds a regular NR baseline that preserves model history
between cuts, plus an every-frame reset control. Async is not a replacement for
the renderer's normal model-history policy.

## Build and software verification

Use an independent checkout and build directory on Windows with Visual Studio
2022 and the Windows SDK. No vendor runtime is needed for these checks:

```powershell
cmake -S tests/nr-async -B out/build/async -G "Visual Studio 17 2022" -A x64
cmake --build out/build/async --config Release --parallel 2
ctest --test-dir out/build/async -C Release --output-on-failure
```

The WARP test uses the D3D12 debug layer and actual compute passes. Its scripted
evaluator can remain blocked while the host continues to submit frames. Checks
cover accumulated motion during inference, immutable snapshots/latest pending
capture, per-link occlusion, colour changes, result age, camera cuts, disabling,
gain bounds, signed colour, invalid vectors, recording failures and completion
admission. It also checks fixed history-memory admission before allocations.

## Manual native NVIDIA comparison

```powershell
cmake -S tests/nr-async -B out/build/async `
  -DTRP_NR_ASYNC_VENDOR=ON -DNGX_SDK="<your NGX SDK>"
cmake --build out/build/async --config Release --parallel 2
python tests/nr-async/run_benchmark.py `
  out/build/async/Release/TRPNeuralAsyncBenchmark.exe `
  "<absolute path to locally supplied nvngx_dlssnr.dll>" `
  out/evidence/native-720 --width 1280 --height 720 --frames 180 `
  --modes off sync async async sync off
```

The executable refuses to start while Skyrim is open. A background monitor
checks for a newly opened game approximately every 100 ms; the producer checks
that monitor before every frame. Process enumeration is outside the producer.
The runner records executable/runtime hashes, CSV samples, final raw RGBA32F
pixels, stdout/stderr, timeout and exit status. It kills only its own child on
timeout. By default a failed child stops the sequence. The explicit
`--allow-shutdown-timeout` option permits further comparisons only when a timed-out
child already logged its result and feature release; the timeout remains a failure
in the evidence. This handles the previously observed normal NGX shutdown stall
without presenting a failed lifecycle as a clean run.

Both modes use the same moving procedural input and native inference parameters.
Samples exclude initial feature setup and sixteen warmup frames. CPU frame scopes
include recording, host submission and host retirement. Host GPU timestamps also
include scheduling gaps, not just shader busy time. Worker evaluation roundtrip
includes CPU recording and inference completion. The one-millisecond requested
sleep between frames is outside those scopes; Windows scheduling can make it
longer. `--pause-us 0` tests a saturated producer with no deliberate pause.
These figures are not whole-game FPS or physical display cadence.

## Limits before game integration

Only one full-resolution world pass with same-size FP16/FP32 colour and guides is
implemented. No UI, game hooks, live controls, dynamic resolution or two-pass
scheduling is attached. Reset/recreation must bracket changes to dimensions,
tuning, placement or producer conventions. The 512 MiB default hard cap covers
prototype histories only, excluding native runtime allocations; live adapter
budget admission is still required for game use. Independent queues may contend
for the same GPU. There is no claim of useful overlap, Skyrim quality, smoother
display timing, AMD support or release readiness from a passing software fixture.

## Paced A/B replay

`TRPNeuralReplayBenchmark` uses the same production `NeuralPass`, one full-size
FP16 world pass, reversed R32 depth and normalized R32G32 current-to-previous
motion. It compares `off`, `regular` (reset only on the first warmup frame and
case cuts), `reset` (reset every evaluation) and `async` (reset every worker
evaluation). Six seconds covers four deterministic 1.5-second cases: world
camera pan, a moving foreground occluder, swaying thin geometry and an exposure
step. Every case transition resets visible/model history. There are no game
images or invented face-quality claims. The additional WARP fixture checks
analytic camera/object guides, reversed depth, cut vectors, thin geometry,
exposure and readback transitions using actual GPU pixels and the debug layer.

```powershell
cmake -S tests/nr-async -B out/build/async-replay `
  -DTRP_NR_ASYNC_VENDOR=ON -DNGX_SDK="<your NGX SDK>"
cmake --build out/build/async-replay --config Release --parallel 2
ctest --test-dir out/build/async-replay -C Release --output-on-failure
python tests/nr-async/run_replay.py `
  out/build/async-replay/Release/TRPNeuralReplayBenchmark.exe `
  "<absolute path to locally supplied nvngx_dlssnr.dll>" `
  out/evidence/replay-timing --rates 30 60 90
```

The producer uses a Windows high-resolution waitable timer against fixed source
deadlines. It records actual intervals, start lateness and deadline misses; a
requested rate is not a physical-cadence measurement. Overdue frames are produced
without dropping their deterministic source indices. Sixteen static warmup
frames and initial async feature creation are outside the paced population.
CPU frame cost includes recording, submit and host retirement; recording and
submit/wait scopes are also separate. Host GPU timestamps include queue
scheduling gaps. Async evaluation roundtrip includes worker CPU recording and
fence completion. Result age is recorded in source frames, nominal source time
and wall time from the captured source frame's CPU start to current host
retirement. The wall value includes capture/recording delays and excludes scanout;
it does not assume missed deadlines have uniform intervals. Frame-weighted evaluation-roundtrip
statistics can repeat the same latest worker sample. CSV evaluation counts mark
the replay interval; stopping can complete a final in-flight evaluation afterward.

The runner preserves the exact executable/PDB and runtime hash, complete CSVs,
phase logs and failed exit statuses. Shutdown gets its own bounded timeout after
the flushed feature-release marker. `--allow-shutdown-timeout` is an explicit
continuation for that phase only and still returns failure after the sequence.
`init-only` and `create-only` provide controls with no evaluation, using the same
NGX bootstrap and pinned runtime admission. The game guard also applies to these
controls. No benchmark is registered as an automatic native CTest.

Memory output contains the fixed prototype allocation and process-local DXGI
usage snapshots after warmup and at the last source frame. DXGI usage includes
runtime, input and other device allocations; it is not an isolated model size or
peak/live-budget admission. Prototype histories remain bounded before allocation.

Capture runs are separate because DMA/readback changes the GPU workload:

```powershell
python tests/nr-async/run_replay.py `
  out/build/async-replay/Release/TRPNeuralReplayBenchmark.exe `
  "<absolute path to locally supplied nvngx_dlssnr.dll>" `
  out/evidence/replay-images --width 640 --height 360 --rates 60 --capture-hz 10 --capture-events
python tests/nr-async/analyze_replay.py out/evidence/replay-images
```

`--capture-events` adds every source frame from two frames before to seven after
the case cuts and exposure step, retaining the base capture rate elsewhere.
This resolves single-frame fallback behavior that a 10-Hz movie can miss.
The GIF follows the irregular source timestamps approximately, with the format's
10-ms time granularity; it is a preview, not a display-cadence measurement.
Capture buffers have a separate 512 MiB bound before allocation. They are
allocated before warmup and copied on the existing host
submission. CPU mapping and raw FP16 file writes happen after all host/worker
retirement. The offline analyzer requires NumPy and Pillow, verifies exact source
frame/time/case alignment, file extent, finite pixels and alpha, and produces a
contact sheet, motion GIF and pixel diagnostics. All image panels use the same
linear-HDR display mapping. Difference panels are signed RGB differences around
gray at 8x scale. Camera-only enhancement-change residuals use the analytic guide
to sample the previous correction; they combine sampling error, model behavior
and scheduling. Pixel disagreement, unchanged-pixel fractions and temporal
residuals are not perceptual scores, confidence masks or game acceptance.
