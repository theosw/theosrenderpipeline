# Development baseline: 0.3.0

The canonical repository is [theosw/theosrenderpipeline](https://github.com/theosw/theosrenderpipeline).
Development starts from the reviewed 0.3.0 release on `main`. Use category/description
branches and a separate checkout/build directory for concurrent tasks.

## Source and release identity

- Release integration: PR #48, merge `7dbc6b1ac9ce316e607ff2571957da066dad0e94`.
- Annotated `v0.3.0`: `55eee70dc72aa6a1fe59e1135ad7383d5c4898cb`, with the same tree.
- Audited 0.3.0 release artifacts were built from renderer source `89ddf3807dbb03757c49e9a87e5b6fd5e837938f`;
  subsequent release finalization changed documentation only.
- A fresh build has its own source revision, DLL hashes and acceptance scope.
  Do not replace the already audited release ZIPs with a baseline rebuild.

## Preserved behavior

Standard includes native NVIDIA integration and NR. Universal adds Ada/Ampere
and experimental Turing frame-generation compatibility while using Standard's
runtime files. Both retain the same plugin identity and NR controls, placement,
passes and tuning. MFG compatibility stays enabled in packaged defaults; NR,
optional NR optimizations and NR hotkeys default off. See [release notes](RELEASE_0_3_0.md).

RTX20 remains experimental. The earlier RTX2060 volunteer run reports x2/x3/x4/x6,
NR and loading recovery; weapon jitter remains unresolved. The combined 0.3.0
Universal ENB/RTX4080 SUPER run has positive feedback with verified identity,
x2/x4, NR and loading recovery, zero TRP errors and two recurring vendor RSYNC
errors. This does not establish other GPUs, Standard-only gameplay, every CS
combination or physical display cadence. HDR is unsupported.

## Build and validation

Follow [BUILD.md](BUILD.md) for pinned source dependencies. Keep local paths in
ignored `CMakeUserPresets.json` and supplied dependencies/runtimes under ignored
`.dependencies/`. Build Standard and Universal into separate directories under
`out/`, with NR enabled in both. Build all configured test targets before CTest.
The 0.3.0 source registers 82 tests per edition; the private offline SKSE Query
harness covers 30 admission cases and never calls SKSEPlugin_Load.

CMake must not deploy or launch. Package assembly, deployment and game acceptance
are separate steps. Private workstation helpers may live under ignored `local/`;
public source builds do not depend on those helpers. Runtime DLLs are not needed
to compile, but a complete test installation retains all eight qualified NVIDIA
runtime DLLs and their notices.

## Historical evidence

The previous AstraRenderPipeline workspace is retained as an evidence archive.
Its source, dated cache paths and old SolFG/Astra mod destinations are not the
current baseline. Carry relevant lessons forward without treating older reports
as current launch instructions. Private raw evidence, supplied runtimes and
symbols stay outside Git. Research PRs remain separately scoped; creating this
baseline does not adopt their experimental changes.
