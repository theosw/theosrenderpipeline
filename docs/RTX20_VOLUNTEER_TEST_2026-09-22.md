# RTX20 Universal volunteer candidate

Separate local candidate based on 0.2.4 hardening `bc0494992aeb84038ef1cd5c58b6d31f89b05e2a`.
No deployment, Skyrim launch or publication is part of this work. Actual RTX20
execution, image quality, performance and presentation remain unverified.

## Changes

- Physical CUDA SM75/LUID identification selects the Turing route. Before
  modifying the provider, matching NVAPI architecture and a GeForce RTX
  2060/2070/2080 product name are required. This excludes GTX16 and unqualified
  Turing products even if they share compute capability or a GPU implementation.
- Missing compatibility permission retains the packaged true default; explicit
  false is respected and explained. Standard still lacks the compatibility path.
- Four unmodified MIT headers from nefh/MFGAmpereUnlock-RenoDx
  `93c5725a534840dc0fa7b1986b216e6b9da3877d` provide SM75 preparation. This fixes
  the 27 programs rejected by the older upstream snapshot. No ReShade host,
  alternative temporal framework or pacing changes are imported.
- Qualified half min/max, packed-half conversion and matrix instructions are
  lowered for SM75. Two K8 matrix operations replace a K16 operation; FP16
  accumulation order is not bit-identical and requires image-quality testing.
- Changed compressed containers are published in bounded chunks through the
  existing guarded transaction. The existing temporal correction is composed
  first, checked against its original hash, then retargeted for SM75.
- The original SM86 planner and its literal writes are preserved. Driver/OS
  failures, DLSS resolver isolation, immutable publication and recreation checks
  remain active. No cached Streamline support override is imported.
- The existing NR feature set, presets, placement, passes and tuning remain
  available. Defaults are fixed x2 with NR off for the first volunteer test.

## Offline evidence

Exact provider: `nvngx_dlssg.dll` 310.9.1, SHA-256
`FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82`.
This matches Standard 0.2.3 and 0.2.4's runtime. The production provider audit
loads the DLLs without initializing NGX, Streamline, CUDA or a graphics device.

- 70/70 provider programs pass production retargeting for SM75.
- The production transaction publishes all checked SM75 programs plus the
  temporal descriptor and architecture gates, then restores their original
  bytes and descriptor. 156 writes; no GPU dispatch.
- The corresponding SM86 transaction passes with its original 208 writes.
  All 70 provider replacements match the original SM86 planner byte-for-byte;
  final temporal PTX matches the previously compiled SM86 fixture exactly.
- NVIDIA ptxas 13.4.59 accepts all 71 final SM75 programs and all 71 SM86
  programs, including each final temporal program. An SM89 positive control
  compiles; a mismatched-target negative control is rejected for each target.
- 68 registered CTests pass across the safe fixture and runtime-layout runs.
  These include 15 Turing and 15 Ampere bridge cases plus upstream PTX behavioral
  checks, driver/OS preservation, nested/thread/SR isolation, repeated creation,
  changed-publication refusal and module-loader coexistence. Expanded minimum-
  architecture requirements cases were rebuilt and rerun after their addition.
- ReShadeAbsent, SourceDLSSGInteropFence and NeuralPeripheralPixels are deferred
  while a separate Skyrim session is active. Their code and the game-hook
  changes are unchanged from the tested hardening base. This is not a claim
  that those fixtures ran against this final build.

Private evidence is under `out/research/rtx20-20260922` in the baseline workspace.
Final package receipts record source commit, renderer hash, Query checks and ZIP
readback separately. The test is not a release recommendation.

## Volunteer acceptance

Follow [the included instructions](../package/RTX20-TEST.md). Establish actual
x2 with NR off, then x3/x4, menu transitions and FG recreation. Save/restart and
test NR separately. Request the GPU/driver, ENB or CS version, full startup log,
package identity and observations of real motion. A multiplied FPS counter
alone does not establish displayed cadence or satisfactory input latency.
