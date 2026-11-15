# AMD FSR API / FSR SDK 2.4.0

The four headers under `Kits/FidelityFX` are unmodified files from AMD's
[FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0),
tag commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`, kept in AMD's folder layout so
their relative includes work unchanged. Their MIT text is in [LICENSE.txt](LICENSE.txt)
and in each header. AMD's whole-SDK licence file is [SDK-LICENSE.md](SDK-LICENSE.md);
it lists the signed DLLs below under the MIT terms.

TRP dynamically loads AMD's signed DX12 loader through its existing fenced
D3D11/D3D12 interop. Each context pins the version the user chose from AMD's own
version query: Image > Mode > FSR 3.1 (`UpscaleType=5`) or FSR 4 (`UpscaleType=6`).
FSR 4 is offered by AMD on supported Radeon GPUs. In Universal only,
`src/FrameGen/FSR4Compatibility.cpp` lets other GPUs with SM 6.6 and native
16-bit support select it: before AMD's first query it verifies both DLLs' exact
SHA-256, AMD Authenticode signature, PE layout, the full admission function and
the INT8 provider/model RTTI, then writes five bytes in memory (RVA 0x99B5) so
AMD's hardware-family check is skipped while its Shader Model 6.6 check stays.
Files on disk are never modified; any mismatch refuses with a reason. The menu
says "INT8 (forced)" only after AMD's context shows the INT8 model was built.
AMD's licence (SDK-LICENSE.md) lists both DLLs under MIT. No FSR frame
generation, replacement swapchain, denoiser or radiance cache is used.

Acquire only the signed files from the official tag (`Kits/FidelityFX/signedbin`):

- `amd_fidelityfx_loader_dx12.dll` SHA-256: `e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb`
  (file version 2.3.0.4555)
- `amd_fidelityfx_upscaler_dx12.dll` SHA-256: `aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace`
  (file version 4.1.2.4555)
- Authenticode signer: Advanced Micro Devices; both signatures verified at acquisition.

Keep the download in its own ignored directory. Verify both hashes before assembling
a package. Place both DLLs in `SKSE/Plugins/TheosRenderPipeline/AMD/`, beside the
supplied `FSR-LICENSE.txt`. The Standard package includes them; Universal uses them
from Standard. Runtime binaries stay out of source Git. The SDK 1.1.4
`amd_fidelityfx_dx12.dll` used by TRP 0.6.4 to 0.7.0 Beta 1 is no longer loaded.

`UpscaleType=5` selects FSR. QualityLevel retains the order Performance, Balanced,
Quality, Ultra Performance, Ultra Quality (custom 1.3x). Inputs are transitioned to
NON_PIXEL_SHADER_RESOURCE and the output to UNORDERED_ACCESS for the dispatch, as
AMD requires for FSR 4, and returned to COMMON afterwards. SDR uses non-linear sRGB
input, exposure 1, render-resolution motion scaled to pixels, TRP's pixel jitter and
camera reset; reverse depth swaps the plane arguments. NR can run before upscaling.

FSR 4 runs only on AMD hardware, so NVIDIA and Intel test machines exercise the
FSR 3.1 provider of the same DLL. FSR 4 image quality and cost need Radeon reports.
