# Optional XeFG tensor helper

This directory builds `trp_xefg_tensor.dll`, the optional Universal XeFG
extension. The renderer owns SDK contexts and presentation resources. This DLL
provides the ported model, eligible tensor dispatches, pacing hooks and checked
retirement through `XeFGTensorAPI.h` and `XeFGPacingAPI.h` in the parent directory.
The tensor preference defaults off. The normal renderer build does not require
these additional dependencies.

The ported model determines image-generation behavior. Tensor execution
accelerates eligible operations in that model. A clean tensor admission refusal
can retain the ported HLSL model. This is distinct from refusing the optional
bundle before loading it, which retains the renderer's ordinary XeFG runtime.
Uncertain initialization or retirement retains resources and requires a restart.
Driver metering probes the off form on the first presenter Present while
metering is enabled, including passthrough before generation starts. Auto VSync
follows the game until that probe succeeds, and when the driver refuses or the
first-output layout is unverified. Failed enables accumulate across successful
off calls; only a successful enable resets the refusal counter. Temporary
no-independent-flip feedback keeps capability admitted so immediate presentation
can recover; it does not establish active metering or physical cadence.
Once configuration attempts start,
retirement still requires an acknowledged off call. A failed off retains the
queue, owner and model instead of allowing replacement.

Verified x3/x4 batches also admit supplemental software cadence with VSync on
when no driver configuration remains outstanding. The sync-specific adapter
samples before the first output's native Present, then anchors deadlines to
Intel's first-output completion stamp with its existing latency credit. This
avoids counting the current batch's presentation waits as frame time; producer
back-pressure can still reduce base FPS. Native x2 cadence and sync-0 driver
metering retain their existing paths.

## Source and asset provenance

- `Extension.cpp`, `TensorEngine.h`, `KernelMetadata.h`, `PacingBridge.*` and
  `pacing/PacingSupport.h` implement TRP's extension boundary, reconstructed
  dispatch contract, telemetry and native-interface admission. The dispatcher
  was reconstructed using static analysis and launch traces of MFGUnlocker
  `1.5.2-xefg134.1`.
- `portable/` adapts MFG-Unlocker's model/shader port and host unlock from
  commit `2b52823e3769c31cf52d3b368c6963cff8fc5344` to Intel XeFG `1.3.4.95`,
  with checked edits and restoration. See [the upstream MIT license](portable/UPSTREAM-LICENSE.txt).
- `pacing/` adapts the v1.6.0 scheduling implementation at
  `ded3614aa2084adb1e04765ecf8f64d33557ed61`. See
  [the pacing attribution](pacing/UPSTREAM-PACING-NOTICE.txt).
- The external DXIL programs, replacement shaders and `tensor.fatbin` were
  extracted unchanged from the identified `1.5.2-xefg134.1` binary. The fatbin
  contains 16 compiled GPU kernels; their CUDA source has not been reconstructed.
  Model weights remain in Intel's separately supplied runtime. The
  [program-data notice](portable/XEFG-PROGRAMS-NOTICE.txt) distinguishes
  Intel-derived program data from upstream MIT source.
- Generated runtime-profile headers contain SDK instruction witnesses and
  relocation bindings. They are external dependencies alongside the GPU assets.
  The portable profile was recovered for the supplied 1.3.4 runtime; the pacing
  profile accompanies the pinned v1.6.0 source adaptation.

[AssetManifest.json](AssetManifest.json) identifies all 68 required dependency
files by size and SHA-256, including the generated headers. Updating a profile or
GPU payload requires a deliberate manifest update and new qualification. Source
attribution does not relabel binary assets as original TRP source.

## Prepare dependencies

Use Windows x64, Visual Studio 2022, Windows SDK and CMake 3.24 or newer. Supply:

1. A qualified helper asset bundle matching `AssetManifest.json`. Existing
   private `private-extension-source.zip` archives contain the required
   `assets/` and `extension/*/*.generated.h` entries. Stage one with:

   ```powershell
   python tools/prepare_xefg_helper_assets.py `
     --archive C:/dependencies/private-extension-source.zip `
     --output C:/dependencies/xefg-helper-assets
   ```

   This reads only the manifest's allowlisted entries, verifies size/hash/CRC,
   and refuses an existing output directory. It does not copy helper source,
   runtime DLLs or SDKs from the archive. The archive must be supplied separately;
   it is not fetched automatically. A bundle prepared elsewhere is also usable
   when every file matches the tracked manifest.

2. NVIDIA's NVAPI SDK headers, including `nvapi.h` and the D3D12 cubin APIs.
   Obtain the SDK from [NVIDIA's NVAPI repository](https://github.com/NVIDIA/nvapi)
   and retain its accompanying license. The helper dynamically resolves native
   NVAPI at runtime; no CUDA toolkit is required to build the helper because the
   GPU kernels are already compiled.
3. [MinHook source](https://github.com/TsudaKageyu/minhook) at
   `8af6b4acae5a9388fd742b56fa79ece89d96f823`, including its license. It is built
   statically in the helper's build tree.

## Build

From the repository root, with paths to your supplied dependencies:

```powershell
cmake -S src/FrameGen/XeFGExtension -B out/build/xefg-helper -A x64 `
  -DTRP_XEFG_HELPER_ASSET_DIR=C:/dependencies/xefg-helper-assets `
  -DTRP_XEFG_NVAPI_SDK_DIR=C:/dependencies/nvapi `
  -DTRP_XEFG_MINHOOK_DIR=C:/dependencies/minhook
cmake --build out/build/xefg-helper --config Release --target trp_xefg_tensor
```

Alternatively, add `-DTRP_BUILD_XEFG_TENSOR_HELPER=ON` and these same dependency
paths when configuring the Universal renderer. That option defaults off;
Standard retains its existing runtime inventory and NR support. The standalone
helper build needs neither Skyrim dependencies nor an installed game.

CMake verifies all asset hashes before compilation and rechecks when they
change. It produces the DLL and matching private PDB in its build directory.
It does not acquire runtimes, deploy files or launch a GPU test or game.
`TRP_XEFG_HELPER_BUILD_FAULT_TARGET=ON` additionally builds the separate
`trp_xefg_tensor_fault` test target under `fault/`; never package that DLL.

The standalone build also provides CPU-only admission and lifetime tests for
the production pacing hooks:

```powershell
cmake --build out/build/xefg-helper --config Release --target TRPXeFGVSyncCadenceTests
ctest --test-dir out/build/xefg-helper -C Release --output-on-failure
```

These check VSync admission/refusal, timing samples, multiplier changes,
reset/retirement and unchanged driver packet bytes without creating a graphics
device or loading Intel's runtime. They do not establish display cadence.

## Runtime bundle and validation

The renderer expects the optional directory
`SKSE/Plugins/TheosRenderPipeline/Intel/XeFG-Tensor/` containing:

- `trp_xefg_tensor.dll` from this build;
- `tensor.fatbin` from the verified asset bundle;
- Intel `libxess_fg.dll` 1.3.4.95, SHA-256
  `752b1491350a29fb2850fa4d0d14f62d79980e0b3cd067d147bb2469c1ec3182`;
- Intel `libxell.dll` 1.3.2.10, SHA-256
  `d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7`;
- upstream, Intel, MinHook and NVAPI notices applicable to the supplied files.

Runtime DLLs and GPU binaries stay outside Git. Preserve the normal Intel
runtime directory too. A build from relocated source is a new binary identity:
keep accepted packages intact and qualify the candidate separately.

Use the explicit fixtures in `tests/xefg` for SDK/tensor/pacing execution,
provider return and retirement. Record actual launches/fallbacks, metering
acceptance and exact runtime hashes. Mock lifetime tests and successful DLL
compilation alone do not establish GPU execution or game acceptance. Output
counts and driver metering do not measure physical display cadence.
