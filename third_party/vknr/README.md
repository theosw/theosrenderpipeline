# TRP Vulkan NR engine

Experimental Universal backend based on the MIT mauri870/DLSSNR-RDNA3 engine,
mochizuki/Overclockers Windows bridge and TRP coord-r005 changes. LICENSE and
NOTICE.txt retain attribution. UPSTREAM-MANIFEST.json identifies the pristine
subset; COORD-R005-MANIFEST.json records the accepted engine snapshot.
SOURCE-MANIFEST.json inventories integration files with LF-normalized SHA-256.
Regenerate it with `python third_party/vknr/tools/source-manifest.py`.

## Reproducible build

Set `TRP_DEPENDENCY_ROOT` and `VCPKG_ROOT`, then use the public `trp-universal`
preset. Its isolated vcpkg install enables the root manifest's `vulkan-nr`
feature: glslang 16.4.0 and SPIR-V Tools 1.4.357.0 are host tools; Vulkan
headers/loader are 1.4.357.0. Explicit overrides leave existing library versions
at the root baseline. To build on another drive:

```powershell
cmake --preset trp-universal -B out/build/universal
cmake --build out/build/universal --config Release
```

Standard does not build the engine or need these tools. Universal never installs
into the supplied dependency snapshot. Source transformations run only in the
build directory; CMake compiles 72 portable, 66 RDNA 3 and five probe SPIR-V
modules and validates every module with spirv-val for Vulkan 1.3. The regeneration
commands are the CMake TRPVulkanNRShaders and TRPVulkanNRProbes targets. No
precompiled SPIR-V or weights are tracked in Git.

`Release/VulkanNR` is the engine payload: DLL, shaders and notices. It uses the
system Vulkan loader, never a packaged loader. At runtime it reads Standard's
original NVIDIA/nvngx_dlssnr_310.9.16.dll as data; no NVIDIA entry point is called
and decoded weights never reach disk. The persistent cache holds pipelines only.
The renderer starts feature creation on a background worker, passes unchanged
colour while preparation is pending and publishes readiness only after completion.
Cancellation releases its reference without joining the render thread; the worker
retains its device and owner through safe completion. Failed retirement quarantines
the complete owner.

## Submission and lifetime

`include/vknr.h` is a private explicit submission ABI. `vknrQuery` matches the
host D3D12 LUID and checks cooperative matrices, formats and fence import.
On AMD, the native fragment/arithmetic probe gates RDNA 3; the portable path
remains available only when its requirements pass. Radeon and Intel are
unvalidated. `vknrCreate` imports dedicated shared textures and a D3D12 fence
as a timeline semaphore, then constructs the small model in RAM.

`Backend::EvaluateNeuralWorld` opens an Interop slot without CPU waiting.
`NeuralPass::Record` finishes preparation and restores borrowed resources to
COMMON. `VulkanNRHost::Evaluate` first prepares an engine command buffer in a
retired slot, packs inputs to shared textures, and calls `Interop::Split` to
submit the producer. It signals ready, calls `vknrSubmit`, GPU-waits for done,
calls `Interop::TrackExternal`, then unpacks into TRP's output on a separate
continuation. The normal reconstruction, model-rate and Compare paths follow.
No CPU waits occur in steady-state evaluation. Initialization, explicit
reconfiguration and recovery retain TRP's existing drain policy.

The three D3D12 slots each retain producer and continuation allocators. A busy
slot returns pass-through before recording. Discard closes only work not yet
submitted; producer and external completion checkpoints survive it. Vulkan
descriptor tickets use actual fence completion, never submission-age guesses.
The host never waits for a value whose Vulkan submit failed. Failed/uncertain
creation, submission or retirement retains the whole owner and shared objects;
recovery cannot recycle them. A failed partial graph constructor also retains
its graph allocation. This deliberate quarantine may consume memory until
process exit after a device/retirement failure.

## Controls and validation

### Background scheduling (ABI 2)

`VulkanScheduling=0` (default) preserves same-frame behavior; `1` selects the
experimental Background Lab row. `NeuralPass::RecordVulkanBackground` polls
`vknrPoll` without waiting. It keeps one network job and three colour/guide
snapshots. A finished result is reconstructed against its capture input and
exposure, then carried forward one or two frames using the existing StoreChange
and Reproject kernels. Compare runs afterwards. First/reset frames use unchanged
input until a valid result exists. Generation changes discard stale results only
after completion. More than two frames old is too old; the prior valid change
continues through the existing reprojection path. Capture gaps above two frames
reset the engine's temporal history; two-frame gaps use ComposeMotion.

Background `VulkanNRHost::Evaluate` submits and signals ready without queuing the
same-frame done wait. `BeginFrame` polls engine fences. `Consume`/`DropCompleted`
queue an already-completed done wait to establish external-image visibility,
then permit resource reuse. Retirement closes the scheduler, drains TRP lists,
and uses `vknrDrain` before releasing the engine. A failed drain retains the whole
owner. CPU waiting is confined to setup/teardown and the standalone harness.

Vulkan timestamps provide inference; separate D3D12 timestamp pairs measure
handoff conversions. Background NR per frame is host work plus amortized network
work, not physical frame latency. Every 120 frames logs report displayed-result
age, arrival age, jobs in flight and discarded results. Same frame remains the
default pending in-game pacing acceptance; queue overlap does not guarantee
driver scheduling or frame-generation presentation evenness.

`TRPVulkanNRBackgroundSmoke` runs the production NeuralPass and host with a
nominal 60-Hz synthetic capture clock. Flags: `--same-frame`, `--large` (3030x854
source / 1515x427 model), `--residual`, `--validation`. It exercises a history
reset, Compare and retirement, reports D3D12 NR-span p50/p95 by submission cadence,
and checks the final output. It does not simulate Skyrim or generated frames.
Run each invocation through the GPU lock. Scheduler CPU tests are in CTest.

`NeuralBackend` is Native=0, HIP/ZLUDA=1, Vulkan=2; no migration is needed.
Both experimental paths use XeSS startup and one small-model pass before
upscaling. Model rate, Compare, input scale and reconstruction remain TRP-side.
Vulkan maps named styles 0..2, local tone/structure, skin and auto skin mask.
Custom styles fall back to Default and are disabled in the menu. Multipass,
intensity, UI correction, object masks, peripheral compression and fused
preparation are disabled for this initial integration.

`TRPVulkanNRHostSmoke <engine-folder> <user-DLL> [--validation]` runs 80 frames
through the production host adapter and Interop, with three recycled slots,
different model/guide extents, RG16F motion conversion and a reset at frame 70.
Test-only environment overrides are compiled out of the renderer. Use the
4080 GPU-lock wrapper for each invocation. Validation uses process-local
`VK_LAYER_PATH` pointing at the toolchain's `bin` directory. The smoke's final
readback checks finite, nonblack, changed output; it is not a quality oracle
or Skyrim acceptance. The test harness alone retries backpressure with a CPU
sleep so it can count 80 successful frames; production returns pass-through.

Expect cold driver compilation on the preparation worker and potentially substantial VRAM use. The
coord-r005 quality and performance measurements are prior standalone evidence,
not measurements of this integration. No Radeon or in-game claim is made.
