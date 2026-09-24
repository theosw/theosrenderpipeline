---
name: arp-game-acceptance
description: Prepare, conduct or interpret a user-requested Theo's Render Pipeline Skyrim game test with package identity and narrowly scoped visual/input acceptance. Use for game-test plans and run reports; standalone build/test requests do not authorize a game launch.
---

# Record a focused ARP game test

Use the user's existing request and authorization; do not ask again for an action
already authorized. Preparing a plan or interpreting old logs does not authorize
deployment or game launch. Resolve links relative to this repository skill.

## Prepare the observation

Read [BASELINE](../../../docs/BASELINE.md) and only the matching entries in
[the topic index](../../../docs/WORKFLOW.md#lessons-and-evidence). Identify the candidate from
current build/package evidence, the symptom being tested and the accepted
behavior the change might affect. Choose a short sequence that can actually
reach and observe that symptom. Do not copy a historical profile or package as
a new default.

For input-close work, distinguish opening, active text editing, the actual close
action, capture release and restored movement. For native UI routing, distinguish
startup loading/spinner, visible main-menu text/highlights, save loading and cell
transition/fade. Use only relevant steps and targeted regression checks. An
unreachable later step is untested. Each observation gets its own result.

## Identify and run the authorized payload

Use [the current deployment workflow](../../../docs/WORKFLOW.md#deployment-and-game-tests)
when deployment is authorized. Select the exact verified ARPPackage directory;
retain lock, rollback and existing-live-INI behavior. Do not deploy through CMake
or replace a guard with a workaround to run a test.

For an authorized launch, follow [manual game checks](../../../docs/WORKFLOW.md#deployment-and-game-tests)
using the selected MO2 installation/profile and the intended package ID. If the
local guarded `local/tools/Launch-ARP.ps1` is available, inspect its interface and
use it. Verify the deployment receipt/immutable payload, source backend, profile/mod
selection and disabled automatic save loading before launching.
Use current session context to resolve the intended profile; if it is unknown,
ask only for that missing choice. The user chooses a save. Do not automatically
edit live settings, switch profiles, load a save or close the game.

## Preserve evidence and acceptance

Before launch, reserve a unique run directory under `out/diagnostics/` and save
the available package/deployment identity, configuration and pre-run logs. After
the observations, preserve relevant logs/captures and their hashes there. Keep
raw private material out of Git; link the run from a concise tracked report.
For a retrospective report, label evidence that was not retained or inspected.

Record the fields relevant to this run:

- Local timestamp/timezone, package ID, main DLL hash and build-input identity.
- Deployment receipt/rollback reference and actual live settings; profile,
  render/output extents, quality/FG/NR/native UI settings when relevant.
- Local versus remote input, physical versus onscreen key, actual close action,
  and any configuration changes between runs.
- Requested steps, observed steps and separate pass/fail/untested outcomes;
  attribute user visual/input reports separately from log-derived conclusions.
- Evidence location, failures, remaining uncertainty and the next needed check.

An entered UI interval or successful Present does not prove visible menu pixels.
F8 closure after text capture releases does not accept End-close during editing.
Physical cadence requires suitable independent measurement; output counts and
remote viewing are not physical-spacing evidence. Standalone results retain
their own scope even when a game test fails.

Update the relevant lesson and index when the result adds reusable knowledge;
update current-state documentation when warranted. Do not turn a candidate into
an accepted checkpoint until the requested behavior has supporting acceptance.
