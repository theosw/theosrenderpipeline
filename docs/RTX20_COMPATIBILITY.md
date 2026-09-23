# Experimental RTX 20 compatibility

Universal's Turing path prepares the supported DLSS-G provider for SM75 before
Streamline initialization, retaining all released 0.2.5 fixes. Standard supplies
the runtime bundle; it does not enable Turing FG.

## Implementation

The original RTX 2060 test selected a raw SM86 convolution outside the prepared
PTX containers. A synchronous module-load guard identified the rejected program
before NVIDIA used a partial object. The corrected path selects the provider's
matching PTX-bearing DL1/DL2 networks. All 39 kernel references must be prepared.
Selection qualifies six complete code bodies and their unwind extents before
publishing two branch changes with the existing transaction. Creation checks
the selection again. Actual-provider CPU execution checked constructor fields,
weights, allocations, dispatch methods and launch arguments. The guard preserves
null API-presence probes and stops on real GPU-module failure. Prerequisite
checks, DLSS isolation and physical architecture reporting remain intact.

Upstream instruction conversion comes from MFGAmpereUnlock-RenoDx; the versioned
headers and MIT notices are retained under extern/MFGTuring. TRP adds provider
selection, failure handling, integration and validation. See
[third-party notices](../THIRD-PARTY.md). No NVIDIA binaries are included in Git.

## Evidence and limits

- Supported provider: DLSS-G 310.9.1, SHA-256
  `FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82`.
- Physical RTX 20 identification is required; SM75 alone also describes GTX 16
  and does not admit it. Existing RTX 30/40 paths are preserved.
- Candidate `b96baa393dbc1996b403cf200c2088f3046b5d5d` has a volunteer RTX 2060
  run on Skyrim 1.6.1170 with ENB/ReShade: 1920x1080 output, 1280x720 DLSS Quality,
  reported 144 Hz and VSync. Across about 25 minutes the runtime reports
  x2/x3/x4/x6 output and recovery after another loading screen.
- NR records 2,758 GPU samples, averaging 37.2 ms for one pass before upscaling
  at 1280x720. This verifies execution, not a new NR compatibility implementation.
- No kernel-load failure or error-level TRP/Streamline entry is recorded.
  Temporary VRAM warnings, focus-related interpolation suspension and some
  transition-associated presentation warnings remain. Weapon jitter is unresolved;
  similar behavior was reported with the volunteer's previous FG setup.

The combined 0.3.0 release has positive ENB/RTX 4080 SUPER regression feedback
with verified loaded DLL identity, x4/NR and loading recovery. Two recurring
vendor RSYNC errors remain. See [release validation](RELEASE_0_3_0.md).
This local Ada run does not add RTX 20 hardware coverage. Other RTX 20 models,
CS on RTX 20, repeated restarts, image equivalence and physical cadence remain
unverified. FP16 lowering changes accumulation order. NR stays optional and
defaults off; HDR remains unsupported.

## Developer checks

Compatibility CTests cover conversion, preparation, rollback, API-presence probes,
driver/OS rejection, scoped resolvers, repeated creation and altered selection.
They use vendor doubles and do not establish physical GPU success. Manual
provider-as-data and CPU-emulation instructions are in
[tests/turing](../tests/turing/README.md). Supply runtimes locally and keep exported
provider bytes/programs out of Git. The host must retain published allocations
until process exit and must not continue after the fatal callback.
