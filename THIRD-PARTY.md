# Attribution and source provenance

This component began in Theo's Render Pipeline (TRP), which now vendors it from
this repository. TRP's [GPL v3 licence and modding/linking exceptions](LICENSE)
apply; separating the code does not relicense it as MIT. Adapted upstream code
is under `extern/`, and `src/` notes where the runtime follows upstream policy.

- **MFGAmpereUnlock-RenoDx**, ImDreamt, mavismmg and nefh: MIT PTX/container and
  architecture work. The SM86 adaptation is based on
  `dd349cdbbae6525188e71fbf2e6d3c648be40db9`; the four unmodified SM75 headers and
  planner checks come from `93c5725a534840dc0fa7b1986b216e6b9da3877d`.
  [Project](https://github.com/nefh/MFGAmpereUnlock-RenoDx),
  [SM86 notice](extern/MFGAmpere/LICENSE), [SM75 notice](extern/MFGTuring/LICENSE).
- **RTX40MFG-Unlock**, Michael Robles: MIT temporal/provider helpers, base
  `4ab7b5e16941e065f81c665b6d7fe2c2e2ec843f`, with the named temporal profile from
  `e13a9841733b0ae43b7215e8c51fe0eb3897816f`.
  [Project](https://github.com/dashdogy/RTX40MFG-Unlock),
  [notice](extern/RTX40MFG/LICENSE).
- **NVIDIA NVAPI**: public architecture-query ABI declarations, MIT, revision
  `87dca625e83fd89a983e19b904e5f3a580da90d2`.
  The original notice is retained in [TRP's full notices](TRP-THIRD-PARTY.md#nvapi-public-architecture-query-abi).
- **This project (Theo)**: the host runtime in `src/`, including guarded
  provider publication, module-path handling, PTX network selection and
  diagnostic/regression work, first developed in Theo's Render Pipeline.
  This is an adaptation built on the projects above, not an independently
  invented instruction converter or a replacement NVIDIA neural network.

The unmodified [TRP notices](TRP-THIRD-PARTY.md) preserve the original licensing
record, including components of the larger renderer that are not in this subset.
NVIDIA SDK headers must be supplied locally under their existing terms;
NVIDIA runtime/model/kernel files are not redistributed by this repository.
