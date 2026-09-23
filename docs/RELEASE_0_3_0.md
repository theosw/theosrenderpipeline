# 0.3.0 — experimental RTX 20 compatibility

Universal adds guarded SM75 provider preparation and selects compatible PTX
endpoint networks on verified RTX 20 cards. Failed GPU-module creation now stops
with a specific diagnostic before NVIDIA can use a partial object. The existing
RTX 30/Ada routes, prerequisite checks, DLSS isolation and all 0.2.5 startup,
settings and runtime-diagnostic fixes are retained.

Standard supplies the unchanged eight-runtime bundle, including NR. RTX 20/30
users must enable Universal after Standard in MO2. NR remains available in both
editions; this release does not introduce a separate NR unlock. Settings defaults,
shaders and vendor DLLs are unchanged.

## Validation

- Both editions pass all 82 standalone CTests and 30 offline SKSE Query/metadata
  cases each. This includes the three GPU fixtures deferred during early work.
- Exact-provider SM75/SM86 preparation, publication and rollback pass. All final
  GPU containers match the earlier volunteer candidate. CPU execution verifies
  the selected PTX networks' weights, allocation and launch contracts and
  unchanged Ampere selection.
- The separate RTX 2060 volunteer run records x2/x3/x4/x6, NR execution and
  loading recovery. See [evidence and limitations](RTX20_COMPATIBILITY.md).
- The combined release has positive user gameplay feedback on Skyrim 1.6.1170,
  RTX 4080 SUPER and ENB. The running DLL and eight runtime hashes match the
  candidate ZIPs. About 33.5 minutes of logs, including startup/loading, record
  x2/x4, 61,620 NR evaluations and four gameplay loading recoveries.
- That combined run has no TRP error/critical entries or recorded sync/NR-GPU
  failures. Two recurring vendor RSYNC errors occur with rendering continuing.
  End open/close and session Apply are logged; restart persistence and individual
  active-edit input actions were not separately tested in this run.

## Binary provenance

The released DLLs are the exact tested artifacts built from
`89ddf3807dbb03757c49e9a87e5b6fd5e837938f`. Release finalization changes only
documentation; no renderer, build configuration, settings or runtime bytes
change. The ZIP marker records that build commit separately from the release
documentation commit. The embedded source revision therefore remains `89ddf3807dbb`.

| Edition | Renderer SHA-256 |
| --- | --- |
| Standard | `3D7A27CF458271FF7CA888EADDF19C76ECBAA88425AE239766456B1B3B48E8D5` |
| Universal | `A72BD36A21700E9476A309E3EB043AB1FF9CDEADD28FE838BE323C61EFFD31A6` |

The reusable developer component is also available in
[dlssg-turing](https://github.com/theosw/dlssg-turing), pinned to the tested
implementation with its tests and original attribution. NVIDIA runtime/model
binaries are not included in either source repository.

RTX 20 support remains experimental. Other RTX 20 models, CS on RTX 20, broader
runtime combinations and physical display cadence remain unverified. The RTX2060
weapon-jitter report is unresolved. NR can be expensive on older GPUs. HDR is
not supported. These limits are not changed by the local Ada regression test.
