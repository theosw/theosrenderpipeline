# Standalone FG kernel oracle

This research executable calls the pinned NVIDIA FG provider on a real D3D12
adapter. It does not launch Skyrim, attach to another process, deploy a mod,
replace kernels, or change the production renderer. All captures are private
build artifacts under this checkout's `out/` directory.

## What it measures

- Direct FG feature creation, evaluation, release and recreation.
- Every interpolated image at x2 and x4, including reset frames.
- Whole-evaluation GPU timestamps and optional NVAPI launch-call timestamps.
- Provider-scoped NVAPI interface resolution, function lifetimes, launch shape,
  shared memory and argument bytes. Original launch batching is preserved.

The executable name contains `nvngx.dll` because the provider's direct-call
contract checks its caller's module name. It is an executable, not an NGX proxy.
It bootstraps the normal NGX loader and rejects an unrecognized provider hash.
The pinned provider is 310.9.1.0, 7,460,976 bytes, SHA-256
`FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82`.
Experimental NVAPI declarations follow NVIDIA/nvapi commit `87dca625`.

x2 uses the native adapter/provider. x4 additionally uses this repository's
existing Ada capability branch and temporal-program patch. No Ampere spoof or
replacement SM86 kernel set is used. The direct oracle has no Streamline wrapper,
swapchain, presentation, Reflex, game UI composition, or NR stage.

## Build and run

Configure from this checkout, supplying the existing NGX SDK path:

```powershell
cmake -S research/fg-kernels -B out/build/fg-kernels -A x64 -DNGX_SDK=<local-sdk>
cmake --build out/build/fg-kernels --config Release
ctest --test-dir out/build/fg-kernels -C Release --output-on-failure
```

Run baseline, observe and profile in separate processes with identical arguments:

```text
nvngx.dll.FGProbe.exe <provider> <new-output-dir> <width> <height> <frames> <baseline|observe|profile> <2|4> <static|move|recreate>
```

`recreate` destroys and recreates the feature every eight real frames after the
previous work has retired. `move` advances a foreground bar four pixels per real
frame over a static grid; normalized motion and depth are supplied separately.
Every generated image is saved as tightly packed RGBA half floats. Full-resolution
captures can consume several GB. Each output directory must be new.

Set `TRP_FG_DEBUG=1` for a separate D3D12 debug-layer validation run. Do not use
debug-layer timings as the performance baseline. The probe fails on validation
errors. It terminates rather than unwinding GPU-owned resources if retirement
cannot be established. A `complete.txt` marker requires successful evaluations,
GPU waits, feature release and NGX shutdown.

```powershell
python research/fg-kernels/analyze.py <baseline-dir> <observe-dir> <profile-dir> --output <analysis.json>
python research/fg-kernels/candidate.py <profile-dir>/launches.tsv --output <candidate.json>
```

The analysis requires byte equality of every input and generated image, expected
capture counts and sizes, finite center-row pixels and plausible intermediate
bar geometry. It also rejects x4 generated images that are all duplicates on a
moving non-reset frame. It is not a full-frame numerical-quality assessment.

## Measurement boundaries

Inputs upload before the measured FG group. All generated evaluations and image
copies are recorded into one command list; there is one GPU retirement wait per
group, never per kernel or per interpolated image. Evaluation timestamps exclude
the oracle's readbacks. Group cost is the sum of its evaluations, counted once.
A launch-call timestamp belongs to its entire batch, not each kernel separately.

`observe` changes only the provider's own resolver import and forwards original
arguments, functions and batches. `profile` additionally inserts timestamp
queries and has measurable overhead. Callback state is process-resident through
NGX shutdown. Unknown function handles, excessive argument sizes, cross-thread
evaluation or unsupported pointer-array launch arguments fail the oracle before
the command list is submitted.

Runs include CPU preparation and disk capture gaps. Clocks, thermals and the
synthetic scene can affect timing. Report the run distribution and compare
uninstrumented controls; these numbers are not Skyrim FPS or physical cadence.
Native Ada results do not establish Ampere behavior.
Readbacks between generated evaluations can affect cache behavior even though
their durations are outside the measured spans. A performance claim also needs
a continuously warmed control with image capture disabled.

## Selected first experiment

The measured candidate is the final **32-channel `k_upscale` followed by
`k_element_wise`** in the motion network. The proposed fused kernel would retain
the original half-precision interpolation operations and rounding, add the
residual using the original half2 operation, and write the existing add output.
That can remove one temporary tensor write/read and one launch without reusing
results from older frames.

`candidate.py` verifies adjacency, matching temporary-buffer address, the fixed
channel count, non-broadcast extents and no direct output/input alias. It reports
later direct references to that temporary. This is **not** a complete liveness
proof or a production admission gate. Some earlier upsample/add pairs write back
into the upsample's input; blindly fusing those could cause cross-thread races.

Before any replacement, recover the exact native program identity, prove all
resource readers and aliases, preserve border/padding behavior and half rounding,
and establish a pure pass-through replacement control. Then compare every image,
total group cost and stage cost through reset, odd extents and recreation. UI,
multiplier transitions, in-game acceptance and RTX 30 validation remain separate.

The existing source files under `extern/RTX40MFG` keep their upstream MIT
attribution and implementation. No NVIDIA runtime, extracted PTX or compiled
kernel blob is part of this research source directory.
