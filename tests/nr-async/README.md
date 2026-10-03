# Standalone asynchronous NVIDIA NR prototype

This experiment is based on the asynchronous capture/evaluate/compose idea in
[RedDukeDev's design](https://github.com/RedDukeDev/dlss5-nr-amd-custom/blob/058e8a998cfba5643081ac5fb29eecc2ad3bd2b5/docs/DESIGN.md).
It uses TRP's existing native NVIDIA feature session and runtime checks. It does
not use ZLUDA, AMD kernels, OptiScaler, or copied upstream source.

The prototype is outside the renderer source list. Building it does not alter a
plugin, installed mod, saved setting or release package. It is disabled by
default in `AsyncPipeline::Config` and must be enabled by its standalone caller.

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

The first version deliberately resets the model's own temporal history on every
evaluation. Both comparison modes use that policy. It is not a replacement for
the every-frame renderer's normal model-history policy.

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
