# Experimental AMD NR runtime

`runtime/` is the complete source used for this branch's separately built
`dlss5nr_runtime.dll`, CUDA gate `nvcuda.dll` and NGX host `nvngx.dll`.
It is based on RedDukeDev/dlss5-nr-amd-custom commit
`620d54fb2c112ae657f18a4b3bfef37d448e473e` (release 1.2.0). See `runtime-source.json` for
source hashes and modifications, `LICENSE` and `runtime/THIRD_PARTY.md` for
upstream attribution. TRP's adapter additions follow the project license.
The older files under `patches/` retain change provenance; they are **not**
an additional patch step for this already modified source.

From the repository root, with Visual Studio 2022 C++/Windows SDK, CMake and
Python 3 installed:

```powershell
python tools/verify_amd_runtime_source.py
cmake -S third_party/amd-nr/runtime -B out/build/amd-runtime -G "Visual Studio 17 2022" -A x64
cmake --build out/build/amd-runtime --config Release
cmake -S third_party/amd-nr -B out/build/amd-nvapi -G "Visual Studio 17 2022" -A x64
cmake --build out/build/amd-nvapi --config Release
```

The runtime build outputs `bin/Release/dlss5nr_runtime.dll` and
`bin/zluda/Release/{nvcuda,nvngx}.dll`. The helper build outputs
`Release/nvapi64.dll`. None of these commands deploys or needs a model, GPU,
HIP installation or vendor runtime to compile. Matching PDBs stay outside
install packages and Git. The renderer builds separately with its public
Standard/Universal presets; both retain NR.

Compilation alone does not produce a runnable AMD NR bundle. Runtime use also
requires the separately supplied signed NVIDIA NR model, matching ZLUDA driver
renamed `zluda_real.dll`, original NVAPI shim renamed `nvapi_trp_upstream.dll`,
native kernel manifest/HSACO files, compatible precompiled translation cache,
and an installed compatible AMD HIP runtime/driver. Preserve each supplier's notices;
the NVIDIA model is proprietary. No vendor binaries or translation cache are
tracked or acquired automatically here.

The host uses `TheosRenderPipeline/AMDNR` below the plugin directory, alongside
its normal NVIDIA and Intel runtimes. Keep the complete identified runtime
bundle together. Adapter selection ABI v1 is required; both HIP and translated
CUDA must match the game adapter, using exact LUID or unambiguous PCI identity.
Diagnostics observe context/device state and never set it to force admission.

This route requires XeSS startup and one world pass before upscaling, with
peripheral compression and fused preparation off. NR defaults off. Async and
Regular (bounded wait) are experimental; Regular composes a later frame and
can reduce raster FPS. C13 adopts upstream camera-depth continuity, packed
residual depth, edge halo removal and result blending (four frames). The host
still requests one pass, history off and the existing bounded post-submit wait;
the new expensive same-frame GPU wait remains off. Tracking debug adds yellow
for removed stale halos. Shader fixtures do not establish a Skyrim improvement.
Shadow pulsing, motion trails and multi-GPU startup remain under investigation.
A successful compile or composed-result counter does not
establish visual quality, gameplay stability or display cadence.

## TRP 0.7.0 integration

Derived from the C14 NR-only host, including capture-age admission (max_age=8),
NR 1.2 tracking, exact adapter selection and retirement. Only Universal admits
the HIP backend. Runtime selection is explicit; HIP and Vulkan cannot execute
in one process. NVIDIA defaults remain native.

The host passes `snippet_path` pointing at Standard's existing
`TheosRenderPipeline/NVIDIA/nvngx_dlssnr_310.9.16.dll`, so AMDNR needs no second
model copy. The model identity is checked before loading. The binary allowlist
in `bundle-assets.json` pins the C14 kernel/cache/ZLUDA inputs; rebuilt TRP runtime
DLLs are identified separately by each release package manifest.

AMD defaults: Regular (bounded post-submit wait), input 50%, NR off. HIP requires
TRP XeSS; its FSR handoff remains unverified and unavailable. Standard retains
native NR but carries no AMDNR runtime bundle. Logs/cache are under
`%LOCALAPPDATA%/TheosRenderPipeline/AMDNR-0.7-310.9.16/`. Runtime identity and
bounded failure/completion diagnostics remain; private startup-hook and FG-host
trace additions are excluded. Detailed CUDA memory tracing remains opt-in.

The binary subset has eleven NR code objects under RedDukeDev's MIT licence.
GPL SR kernels, d4r command-list code, OptiScaler proxies, DXVK and installers
are excluded. Retain the upstream notices and both ZLUDA licence texts. NVIDIA's
model and the matching translation cache retain their respective upstream terms;
they are not re-licensed by TRP.
