# AMD FidelityFX API / FSR 3.1.4

The four headers in `include/ffx_api` are unmodified files from AMD's
[FidelityFX SDK 1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v1.1.4),
tag commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. MIT text is in
[LICENSE.txt](LICENSE.txt); each header also retains its notice.

TRP dynamically loads the official DX12 backend through its existing fenced
D3D11/D3D12 interop. No third-party DX11 port, FSR frame generation, replacement
swapchain or ML upscaler is used. FSR 3.1.4 is a compute temporal upscaler usable
on Pascal without tensor cores. AMD's signed SDK runtime avoids a local shader
compiler toolchain dependency and keeps both editions on identical shaders.

Acquire only the official release asset:

`https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip`

- ZIP SHA-256: `0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952`
- `bin/amd_fidelityfx_dx12.dll` SHA-256: `12a5081257ec95b0b53ad51b4a87fb3c03f97fe0bbb59f9496968f8d50ef93a6`
- DLL file version: `1.0.1.41314`; API provider: FSR 3.1.4.
- Authenticode signer: Advanced Micro Devices; signature verified at acquisition.

Keep the download and extraction in their own ignored directory. Verify both
hashes before assembling a package. Place the runtime at
`SKSE/Plugins/TheosRenderPipeline/AMD/amd_fidelityfx_dx12.dll`, beside the supplied
`FSR-LICENSE.txt`. Release packages include it in both editions. Runtime binaries
and original archives stay out of source Git.
No additional AMD sample, Agility SDK, frame-generation or compiler DLL is needed.

`UpscaleType=5` selects FSR; existing values and RTX defaults remain unchanged.
QualityLevel retains the order Performance, Balanced, Quality, Ultra Performance,
Ultra Quality (custom 1.3x). Mode switches can reuse the existing render extent;
quality changes require a restart. SDR uses non-linear sRGB input, exposure 1,
render-resolution normalized motion scaled to pixels, TRP's pixel jitter and
camera reset. The active camera supplies finite near/far and vertical FOV;
reverse depth swaps plane arguments as AMD requires. TRP's NR can run before
reconstruction. Native UI composition and TRP HDR output remain downstream.
Reactive and transparency masks are currently absent; particles and disocclusions
need game quality review. Nonlinear SDR is the primary qualified input path.

Build/run the standalone comparison with `cmake -S tests/fsr -B out/build/fsr`.
`TRPUpscalerBenchmark <Intel runtime dir> <AMD runtime dir> <capture dir>` uses
production backends and fixed 753x424 -> 1280x720 / 1129x635 -> 1920x1080 inputs.
Its synthetic captures and timing scopes do not establish Skyrim quality or
whole-frame performance. No test downloads a runtime or deploys into a game.
