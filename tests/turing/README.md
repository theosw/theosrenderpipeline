# RTX20 module-loading investigation

This candidate adds a synchronous failure boundary for the first real
`NvAPI_D3D12_CreateCuModule` failure in the Turing provider. It does not change
the lowered GPU programs or establish working RTX20 frame generation.

The first RTX2060 volunteer log reports 25 module-load failures (status -1),
then failed DL4RT network creation, while outer NGX creation still returns
success. A later worker crashes writing through a null object. Streamline
delivers these messages on a different thread; its log callback is too late
to be the sole failure boundary.

The provider-local resolver intercepts only the CuModule query ID. Architecture
queries and other modules keep their original functions. The intentional null
API-presence probe is forwarded unchanged. A real load failure logs the call,
program index, container metadata and a bounded FNV-1a fingerprint, then invokes
the terminating host callback before returning to the vendor. No partial FG
object is recovered or destroyed. RTX30 retains its original two resolver
imports; the third import is installed only on the Turing path.

## Checks and limits

- `ProviderAudit` exports the final 70 containers plus the temporal clone and
  checks production publication/rollback. Exported fatbins stay private.
- `CuModuleProbe` manually creates a local D3D12 device, loads these containers
  through NVAPI, destroys the module handles, and never dispatches a kernel.
- `ProviderGuardProbe` checks the exact qualified 310.9.1 thunk contract and
  invokes its null presence probe through the production resolver. It initializes
  NVAPI, but creates no graphics device and performs no GPU dispatch.
- Runtime fixtures cover successful forwarding, the null presence probe,
  synchronous failure termination, a null handle with a success result,
  isolation, recreation, and refusing an outer success after internal failure.

The qualified provider SHA-256 is
`FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82`.
Static analysis places its module-load call at RVA `0x3088b`, calling the thunk
at `0x2960`. Its API-presence probe is at `0x32262`. These are investigation
coordinates, not hard-coded production hook offsets. The production IAT slot
is discovered by import name.

On the local RTX4080 SUPER, all 71 SM75 and 71 SM86 containers load through
NVAPI and CUDA, and their extracted PTX also loads through CUDA (284 checks).
The installed driver JIT also compiles all 71 SM75 programs explicitly for
Turing using `CU_JIT_TARGET=75`. This weakens a generic malformed-container or
illegal-PTX explanation. It does not test physical RTX20 module loading,
inference, pixels or presentation. The volunteer's driver-side rejection is
still unresolved. An alternative vendor loading path must not be enabled
merely because its entry point exists.

The ABI and query ID are declared by NVIDIA in
[nvapi.h](https://docs.nvidia.com/nvapi/nvapi_8h_source.html) and
[nvapi_interface.h](https://github.com/NVIDIA/nvapi/blob/main/nvapi_interface.h).
