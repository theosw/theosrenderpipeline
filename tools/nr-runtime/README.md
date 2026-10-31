## GTX 10 production inventory (0.6.0 draft)

Universal adds `nvngx_dlssnr_310.9.16_pascal_v1.trpk` beside the existing NR assets.
`package/NR-CompatibilityAssets.json` pins the additive inventory and required
original 310.9.16 runtime. Standard installs no compatibility kernels. Supply the
pinned Pascal-only local input to `stage_pascal_pack.py --edition Universal
--input <sm-general-pascal.trpk> --output <new-staging-directory>`; merge that
supplement into Universal while retaining all existing assets. Never commit the
pack bytes. Runtime admission verifies the whole-file pin and embedded manifest
before patching or vendor initialization. The pack includes only sm_61, with
SM-independent ticket-ring scratch slots, and remains experimental under the
in-order dispatch correctness assumption accepted for this compatibility path.

Ordinary Universal users enable NR with Automatic and Apply. No research INI or
environment variable is needed. `TRP_ENABLE_PASCAL_RESEARCH=ON` remains a separate
developer build for arbitrary packs, mode 2 and research instrumentation; normal
release builds ignore those controls. Driver detection uses NVAPI's
`SYS_GetDriverAndBranchVersion`; if unreadable, the rendering adapter's DXGI UMD
version is decoded using Windows NVIDIA numbering (32.0.15.8278 -> 582.78).
Unknown versions refuse. CUDA version is checked independently and is never
interpreted as an NVIDIA display-driver version.

# Neural Rendering compatibility kernels

Standard retains the existing NR 310.8 DLL and original NVIDIA NR 310.9.16.
Universal supplies `nvngx_dlssnr_310.9.16.trpk`, a 12,215,592-byte kernel pack,
plus the retained community INT8 v2 DLL for RTX 20/30. The pack replaces the
separate full community and standalone RTX 40 310.9.16 DLLs in the release.
It contains GPU code and metadata, without duplicating the runtime's model
weights or host DLL. Explicit user-supplied runtime paths remain supported.

## Reproduce the combined pack

Run without Python optimization, using locally supplied inputs:

```powershell
python -I tools/nr-runtime/build_compat_kernels.py --official <original-310.9.16.dll> --compat <community-310.9.16.dll> --ada-pack <qualified-rtx40.trpk> --cuda <CUDA-root> --output <folder>/nvngx_dlssnr_310.9.16.trpk
```

The builder verifies every input's complete SHA-256 before parsing:

- Original NVIDIA 310.9.16:
  `2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F`.
- ShortFuse's community compatibility runtime:
  `CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED`.
  This is the reference reproduced by [openNR](https://github.com/clshortfuse/openNR)
  at `57ca170cb836f549f0e64544f282d535666b0f7d`; its source builder accepts the
  original NVIDIA file. The complete compatibility DLL is a build input only.
- Qualified TRP RTX 40 pack:
  `ACD2B6DD5526A71FFD5B34F78A9B7230DFDD57D5F79C581D10A876EB130AF542`.

Use the pinned CUDA 13.4 tools reporting V13.4.92. The builder extracts each
of 15 bundles, keeps the community SM75/SM86 cubins, TRP SM89 cubins and
NVIDIA SM120 cubins/PTX, then compresses them into one pack. Read-back must
reproduce every selected cubin and original PTX exactly. It also checks the
original and community host-admission sites. The qualified pack SHA-256 is
`751006ADB39C8DF0A7580EE095F8E605719AAA62832D6524DE30192E4A668FD3`.
An `--unqualified` output is explicitly marked and refused by the renderer.
No input DLL is loaded, edited or installed by either builder.

The retained `build_rtx40_kernels.py` produces the qualified RTX 40 input:

```powershell
python -I tools/nr-runtime/build_rtx40_kernels.py --official <original-310.9.16.dll> --cuda <CUDA-root> --output <folder>/nvngx_dlssnr_310.9.16_rtx40.trpk
```

It compiles NVIDIA's PTX for SM89 with narrow, audited instruction lowering.
`build_rtx40.py` retains the standalone DLL builder for manual comparisons;
that full DLL and the RTX 40-only input pack no longer ship in the release.
Source licensing and kernel/runtime attribution are in THIRD-PARTY.md.

## Runtime installation

Install the combined pack beside `nvngx_dlssnr_310.9.16.dll`, under
`SKSE/Plugins/TheosRenderPipeline/NVIDIA/`. Universal's Automatic selection
tries that original runtime on RTX 20/30/40. RTX 50 uses it without a patch.
RTX 20/30 use FP16 tensor-core fallback for the FP8 pipeline; RTX 40 keeps
native FP8. Missing, damaged or refused packs safely fall back to legacy
INT8 then 310.8 on RTX 20/30, or 310.8 on RTX 40. INT8 v2's exact known-bad
hash is refused on RTX 40. Standard links an installer refusal stub, never
modifies NR memory and keeps 310.8 on older GPUs.

Universal verifies both complete file hashes, loaded headers (accounting for
ASLR), original kernel bundles, all 38 patch sites and signed displacement
reach. This includes all four host architecture entries and two bundle
lengths cached by CRT initialization. It installs only into a freshly loaded
module before NGX initialization. A resident module is accepted only when this
installer owns and verifies its recorded installation. Initialization across
passes is serialized, and sibling assets use MO2's merged Data path.

The pack allocation becomes read-only and remains resident with the pinned
module for process lifetime. Writes are verified and the instruction cache
is flushed. Protection restoration accounts for private copy-on-write image
pages. Failure restores original bytes and protections before fallback;
unconfirmed rollback or changed installed storage blocks use until restart.
No runtime file on disk is modified.

`NeuralRuntimeKernels` CTest covers planning, malformed packs, displacement
limits, every partial write, protection failures, rollback, competing edits
and GPU admission. Supplying absolute runtime and pack paths to its manual
probe separately exercises real Windows mapping, pinning, concurrent reuse
and tamper detection; it does not initialize NGX. The optional benchmark in
`tests/nr-peripheral` exercises production feature creation and pixel readback
with `TRP_NR_VENDOR_BENCHMARK=ON` and `TRP_NR_ADA_KERNELS=ON` (the option keeps
its historical name). Manual probes are never launched by a build or CTest.

Byte-identical cubin preservation and static architecture checks do not
establish local RTX 20/30 hardware acceptance. Keep standalone pixel results,
actual game execution, user visual feedback and physical cadence separate.

## Pascal research builds

`TRP_ENABLE_PASCAL_RESEARCH=ON` is an opt-in CMake option, off in both public
presets. It includes the research installer in Standard while preserving
Standard's normal RTX refusal policy. Release builds ignore the research INI
and environment variables. On a GTX 10 (compute 6.1), research startup uses
XeSS and the Intel presenter without initializing NVIDIA SR/FG. NR remains an
independent, optional feature; the normal NR enable setting and controls apply.
RTX startup is unchanged. The manual vendor benchmark also permits explicit
SM89 reference checks.

Add to `Data/SKSE/Plugins/TheosRenderPipeline.Diagnostics.ini` and restart:

```ini
[NeuralRenderingResearch]
EnablePascal=1
PascalPack=NVIDIA/c24-launch.trpk
```

Relative packs resolve under `Data/SKSE/Plugins/TheosRenderPipeline/` through
the merged Data view; absolute paths work too. `TRP_NR_PASCAL_RESEARCH=0|1`
and `TRP_NR_PASCAL_PACK` override these keys. Default is off. Use original
310.9.16 (`nvngx_dlssnr_310.9.16.dll`); research Automatic selects it without
falling back to an incompatible legacy runtime. Native UI composition and
the usual NR prerequisites still apply. Keep the signed runtime and generated
packs outside source control. This path has no in-game acceptance yet.

The Pascal builder accepts repeatable
`--launch KERNEL:BXxBYxBZ[:GXxGYxGZ][:SHARED]` arguments, including
`KERNEL:128x1x1::0` to set shared bytes while retaining the submitted grid.
Names must identify a native replacement supplied with `--native`. Fixed grid
requirements are resolution-specific; c24 overrides only the block dimensions.
Use `KERNEL:32x4x1:*1x1x2` to retain the submitted x/y grid and double z.
Each multiplier must be 1..64. The JSON report uses `gridMultiplier` instead
of `grid` for these rules. Multiplication is checked against CUDA's x limit
of 2147483647 and y/z limits of 65535 before any chain reaches the driver.

With no launch rules or recorded bounds, the writer emits byte-identical TRPNRK1 v1.
Rules or bounds select TRPNRK1 v2: the original 48-byte header, 15 bundle records,
alignment and fatbin payloads are unchanged, except header version at byte 8.
Immediately after the last fatbin comes `TRPLCH1\0`, uint32 schema version 1,
uint32 record count, then variable records. Each record has eleven little-endian
uint32 fields: bundle index, architecture mask (1=SM61, 2=SM89), ASCII name
length, flags (1=fixed grid, 2=dynamic shared, 4=grid multipliers), block xyz, grid xyz, shared bytes;
then the name without a terminator, zero-padded to four bytes. Flags 1 and 4
are mutually exclusive. Absent optional
fields are zero. Unknown versions/flags, duplicates, invalid dimensions,
truncation and trailing bytes are refused. Old readers safely refuse v2;
new readers retain v1 RTX and Pascal compatibility.

Before NR initializes, a runtime-local resolver hook covers CU module/function
creation/destruction and both chain APIs. Requirements must bind to the stated
bundle and a known function before creation commands submit. Every launch
preserves the chain's count/order and all parameter pointers; only descriptors
change. There are no added GPU commands or barriers. Unavailable APIs, resolver
bypass, missing names, unknown handles and driver refusal leave NR off. Empty
NVAPI capability probes keep their original result and cannot poison admission.
Hooks remain with the pinned runtime across feature recreation; destroyed
functions/modules remove bindings, preventing handle reuse from inheriting rules.

Research uses the runtime directly without initializing public NGX core. Its
last retired feature calls the runtime's own `D3D12_Shutdown1`; simultaneous NR
passes share ownership. A failed release retains initialization. The manual
benchmark calls public NGX shutdown only after successful public initialization.
Normal RTX ownership remains with its existing host.


`--launch-bounds` writes TRPLCH1 schema 2. Bounds are recorded by default
when any native kernel has maxThreads < 1024 or a reqntid, and always for
multiplier rules. `--no-launch-bounds` explicitly omits them only to reproduce
historical pack bytes; it never disables build-time validation. The build
report records `launchBoundsRecorded` and `unknownOriginalBlockCount`.
Each record extends the eleven schema-1 words with two five-word bounds, first
SM61 then SM89: maximum threads, required block xyz (zero if absent), maximum
dynamic shared bytes. A bound-only record has zero block/grid/shared/flags;
its name is checked only when launched. Schema-1-only readers refuse schema 2.
The builder reads each native function's ELF `.nv.info` maxntid/reqntid, `.text`
register count and `.nv.shared` size. Register bounds use 256-register warp
allocation and four register partitions (64K registers); static plus dynamic
shared memory is limited to 48 KiB. After rewriting, the runtime checks the
selected architecture's bound before forwarding the complete chain.

Bounds extraction and original-PTX block validation run even without bounds
metadata. A native entry exceeding its compiled bounds needs a valid launch
rule. NVIDIA entries with no maxntid/reqntid have unknown original blocks;
the builder warns and reports their count. It also names every restrictive
native entry without a launch rule. Schema-2 packs still check
the actual runtime launch. No ELF parser runs inside the DLL. No rules/bounds
still emits the same v1 bytes; old-style rules without bounds retain schema 1.


A rejected chain latches failure and drops every later chain in that Evaluate.
The hook returns a benign NVAPI status so NVIDIA can unwind without fail-fast;
FeatureSession checks health immediately after Evaluate and never evaluates a
failed session again. Rejected command lists are marked non-executable. Both
game call sites discard the list before any submission, keeping fences/slots
unchanged; the queue execution gate also refuses rejected lists. The benchmark
closes/discards the list, retires the feature and reports NR_OFF with no frame.
The game's existing unknown-recording-failure policy still latches the entire
backend until restart: it does not promise continued native/XeSS presentation.

## SM-independent scratch allocator experiment

`build_sm_general.py` verifies `c37-inputs.lock.json`, prepares the twelve selected
swinmh CUDA sources, and builds a Pascal-only pack plus a separate SM89 reference
pack. The selected external research inputs are required; no vendor binary or
generated PTX/cubin is stored here. Run in a new output directory:

```powershell
python -I tools/nr-runtime/build_sm_general.py --research-root <trp-pascal> --official <310.9.16.dll> --cuda12 <CUDA12.9> --cuda13 <CUDA13.4> --output out/sm-general
python -I tools/nr-runtime/test_sm_general.py --output out/scheduling-results.json
```

The ticket ring in `sm_general_slots.cuh` removes raw SM-ID ownership and gives
disjoint scratch lanes with generation-controlled reuse. Producer polls precede
scratch reservation. This is **an allocator candidate, not a complete scheduling
fix**: native/original cross-dispatch polls, same-launch split-K/Z waits, cache
writer progress and shared-module invocation ordering remain release gates.
The synthetic suite includes counterexamples for those waits; its 1–32 logical
SM-count matrix does not emulate physical GPUs or WDDM. No production admission
or runtime policy is changed by this tooling.

`sm_general_probe.cu` is a developer CUDA occupancy/worker-pressure fixture.
`run_sm_general.ps1` is a lock-respecting test-PC runner for interleaved c37/new
timing and full RGBA float32 byte parity, including separate untimed 40-frame
exports. Stage only in a dedicated `C:/trp-work/runs/` directory with an
`inputs.json` SHA256 inventory. Every GPU invocation uses the existing harness
lock; a failure or teardown hang stops the campaign. `-SkipProbe` is appropriate
only when that separately recorded probe is unavailable or blocked; it does not
count as passing the GPU allocator test.
