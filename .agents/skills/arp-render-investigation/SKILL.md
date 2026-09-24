---
name: arp-render-investigation
description: Investigate Theo's Render Pipeline rendering or input regressions and compare source host behavior with identified reference runtimes. Use for hook, frame-phase, UI, resource-lifetime or runtime-parity diagnosis; routine edits and general Skyrim modding do not need this procedure.
---

# Investigate an ARP regression

Use the current user's objective and authorization. This procedure does not
authorize a deployment, game launch, invasive attachment or unrelated refactor.
Resolve links relative to this skill in the repository's `.agents/skills/` tree.

## Establish the comparison

Read [BASELINE](../../../docs/BASELINE.md) for the current state and select only
the matching entries in [the topic index](../../../docs/WORKFLOW.md#lessons-and-evidence).
Identify the failing package/configuration, exact visible symptom and the last
accepted behavior with its limits. A dated package in a report is evidence, not
proof of what is now installed. Reuse existing captures before collecting more.

Separate observation from explanation. Record what each artifact can show:
static call-site agreement, live event ordering, bound resources, actual pixels,
or a user-observed input/visual result. Choose the smallest measurement that can
distinguish the leading explanations within the authorized task.

For performance work, hold scene, resolution, settings and warmup comparable;
record GPU/driver, shader setup and exact binaries. Distinguish GPU work, CPU
frame time, output counts and physical cadence. Check hitches/tails and image
quality before recommending a change from its average timing. Use private
lessons under `local/knowledge/lessons/` when available, without requiring them
for an otherwise answerable public-source investigation.

## Recover the relevant contract

Trace producer completion, evaluation, UI entry and Present independently.
For target/viewport failures, include context identity, resource identity and
extent, all relevant attachments, binding order and restoration. For input,
compare the game cursor, engine viewport and menu drawing coordinate systems.
Use the corresponding lesson for the incident-specific details.

When comparing binaries, record exact version/hash and distinguish image VA
from RVA and version-specific relocation IDs. Verify surrounding instructions,
original-call chaining, arguments and return behavior; decompiler names or a
nearby byte match alone do not establish a hook contract. Reconfirm the relevant
sequence in the intended reference version before transferring older findings.
Packed on-disk Skyrim text is not evidence of the live call bytes. Treat
unavailable live evidence as a stated limit rather than an invitation to launch
or attach outside the user's request.

Do not equate a historically named AMD/PureDark source file with an inactive
component: shared NVIDIA host dependencies still pass through that code. Preserve
retirement-before-release and failure retention when changing resource owners.

## Make the result attributable

Keep the change tied to the measured contract. If another independent defect
appears, document it separately instead of combining an unrelated correction.
Add a behavioral fixture when it can reproduce the failure meaningfully; reuse
the current [build/check workflow](../../../docs/BASELINE.md#build-and-validation), then rerun
only affected checks after subsequent edits. A static audit without source
changes needs evidence review, not a renderer rebuild.

Report the supported cause or remaining hypothesis, the correction and its
verification limits. Update the relevant lesson after a reusable finding using
the index's maintenance guidance. A passing fixture leaves game behavior pending
unless a matching game run was actually performed and accepted. If further
evidence is needed, state the specific next observation without claiming a fix.
