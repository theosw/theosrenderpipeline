---
name: trp-release-preparation
description: Prepare or audit a Theo's Render Pipeline release candidate, Standard/Universal package pair, or release handoff. Use for packaging and release readiness; ordinary source edits do not require this workflow.
---

# Prepare an identified TRP release

Read [BASELINE](../../../docs/BASELINE.md), [BUILD](../../../docs/BUILD.md) and
the relevant [workflow](../../../docs/WORKFLOW.md#build-and-release-identity).
Use the user's requested version and existing authorization. Packaging does not
itself authorize deployment, game launch, upload or merge; do not ask again when
the specific action is already authorized. A release audit may be read-only.

## Establish the candidate

Identify the source checkpoint, edition split and existing accepted binaries.
For a new build, confirm the clean committed tree, Git author, local dependency
paths and independent edition directories before configuring. Freeze build
inputs until both editions finish. Preserve NR in both editions and all current
Universal compatibility routes, runtime checks and packaged defaults.

Verify the embedded source revision, release/SKSE/Windows versions, plugin
identity, public exports and DLL hashes. Preserve matching private symbols.
Record the tests actually run and their configuration; check the current test
registry rather than treating a historical count as the target. Use the available
offline Query/metadata harness with expectations matching the requested version.
Do not call SKSEPlugin_Load outside its intended game host.

When only documentation changes after acceptance, verify unchanged renderer,
build, shader, settings and runtime inputs; retain accepted DLLs instead of
rebuilding solely to label the ZIP. Record renderer build source separately
from the final tag/merge/documentation reference. A rebuilt DLL is a new identity.

## Assemble and check the pair

Use current versioned staging/assembly helpers when available, after inspecting
their interfaces and version pins. Local installations keep these under
`local/tools/`; public clones need the corresponding qualified local inputs.
Do not borrow a historical assembler's hard-coded version or remove NR/runtimes
to make packaging succeed. Report a required input that is actually unavailable.

Preserve the complete Standard runtime bundle and applicable notices; Universal
overlays its renderer and retains NR. Verify source/build identity, manifests,
runtime hashes, common settings and the merged Standard-plus-Universal file view.
Honor preserve/replace/archive policies: existing INIs survive deployment, private
symbols remain outside install ZIPs, and no unrelated mod overrides enter a package.

Put the edition in the filename's letters-and-spaces prefix before the version
so MO2 can distinguish Standard and Universal. Check actual installer behavior
when installer metadata changes; XML validity or ZIP extraction alone is not MO2
acceptance. Record archive hashes and verify file lists, CRC/readback and Windows
extraction. Do not overwrite an identified release archive silently.

## Report readiness within its evidence

Separate build, standalone tests, real NVIDIA execution, actual loaded-package
identity and user gameplay/input acceptance. MFG acceptance needs a supported
multiplier above x2. A generated-output counter is not physical display cadence.
Carry current hardware/driver/shader and known-issue limits into release notes;
do not turn a scoped volunteer result into a whole-generation guarantee.

Deliver exact package paths/hashes, source and symbol provenance, relevant
validation, remaining limits and publication status. For an authorized game test,
use the project game-acceptance skill if available, retaining guarded deployment
and user-selected saves. If publication is requested, ensure the final PR/tag
describes the verified implementation and that main receives the reviewed release.
Keep deployment, uploaded assets and source publication distinct in the result.
