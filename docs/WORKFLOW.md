# Development workflow

Use [BASELINE](BASELINE.md) for the release starting point, [ARCHITECTURE](ARCHITECTURE.md)
for owners and boundaries, and [BUILD](BUILD.md) for source dependencies and build commands.
This guide records recurring practices; it does not authorize deployment or game launch.

## Changes and measurements

Keep a change tied to one defect or hypothesis. Work on a category/description
branch with an independent build directory. Inspect current source before adopting
an old PR or experiment: a change may have been rebased, cherry-picked or superseded.
Keep main aligned with reviewed release integrations; do not assemble a new baseline
from an arbitrary collection of open PRs.

For performance comparisons, identify the exact build, runtime, hardware, driver,
resolution, scene and settings. Control warmup and compare matched runs. Separate
CPU frame time, GPU work, generated output counts, display pacing and image quality;
include tails/hitches where relevant. A lower average cost does not establish better
smoothness. Temporal reuse previously reduced average work but added microstutters.
Use [the investigation skill](../.agents/skills/arp-render-investigation/SKILL.md)
for rendering/input diagnosis. Rerun only affected checks after subsequent edits;
routine documentation changes do not require rebuilding the renderer.

## Build and release identity

Commit the intended source checkpoint before configuring a release build. Verify
the Git author, clean tree, edition and embedded source revision. Keep configure
inputs stable until both edition builds finish. Use separate Standard and Universal
build directories with NR retained in each. Build the relevant test executables
before running CTest; a registered test is not proof it ran. Release assertions
must remain active under NDEBUG.

Record actual source commit, DLL hash, private symbols, dependency/runtime identity
and test results. Source equivalence is not binary identity. When documentation or
a merge commit follows the accepted renderer build, record the build commit and
release reference separately and verify that runtime/build/default inputs did not
change. Never replace an accepted ZIP silently with a rebuilt DLL.

Use [release preparation](../.agents/skills/trp-release-preparation/SKILL.md) when
assembling or auditing a release pair. Preserve the current Standard runtime bundle,
including NR, and Universal's compatibility implementation. Validate the combined
MO2 file view, notices, settings, filename-derived edition names and extracted hashes.
Keep manifests, PDBs and raw evidence outside install ZIPs where appropriate.

Local installations may provide guarded helpers under `local/tools/`: source staging,
candidate assembly, deployment and rollback. They are private workstation tooling,
not prerequisites for compiling public source. Read their current interfaces before
use; do not reuse version-pinned historical assemblers or invent missing runtime
inputs. A complete installation requires the qualified locally supplied runtimes.

## Deployment and game tests

Use authorization already present in the session. A request to build or review a
package does not imply deployment, game launch or publication. Before a real
deployment, check both Skyrim and MO2, identify the intended installation/profile,
and retain the destination lock, integrity verification, live-INI preservation and
rollback receipt. Do not overwrite another task's test or profile changes.

For an authorized launch, use MO2 and let the user choose a save. Verify what actually
loaded, including the renderer DLL, vendor runtimes and winning INI. MO2 Overwrite or
another enabled mod can win over the packaged defaults. Inspect configured, requested,
effective and saved settings separately. Settings changes need an appropriate
save/restart round trip; reinstallation should not be the default troubleshooting step.

Choose a short test sequence for the changed behavior. Where affected, include
startup, a loading transition, relevant menu/input behavior, supported MFG above x2,
NR placement/pass behavior, or restart persistence. This is not a mandatory full
matrix for every edit. Follow [game acceptance](../.agents/skills/arp-game-acceptance/SKILL.md).
Do not infer every checkbox, hardware combination or physical cadence from a general
"looks good" or a higher generated-FPS counter.

## Lessons and evidence

Consult only relevant topics. Public source and scoped compatibility reports remain
available in fresh clones:

| Working on | Starting points |
| --- | --- |
| Resource ownership and ordering | [Architecture](ARCHITECTURE.md) |
| Startup hooks and runtime paths | [Hook compatibility](STARTUP_HOOK_COMPATIBILITY.md), [0.2.4 hardening](RELEASE_0_2_4_HARDENING.md) |
| Save/Apply and menu state | [Settings recovery](SETTINGS_ACTION_RECOVERY.md), [independent NR controls](NR_PASS_CONTROLS.md) |
| External rendering/input | [CS](COMMUNITY_SHADERS.md), [ReShade](RESHADE.md), [Nolvus](NOLVUS.md) |
| Hardware compatibility | [RTX20](RTX20_COMPATIBILITY.md), [RTX30](RTX30_TESTING.md), version-specific Skyrim reports |
| NR performance options | [Peripheral compression](NR_PERIPHERAL_EXPERIMENT.md), [combined preparation](NR_COMBINED_PREPARATION.md) |

When available, the private lesson index in `local/knowledge/lessons/README.md`
links richer investigation reports and raw evidence. Those dated reports are
historical, not commands to reuse old paths. If an artifact is unavailable, state
that limit rather than treating its report as a fresh verification.

After a meaningful fix or disproved hypothesis, update the existing lesson: symptom,
cause or remaining hypothesis, correction, evidence, configuration and acceptance
limits. Replace superseded active conclusions and retain a brief evidence link.
Add a regression check when it can observe the failure; avoid tests that only mirror
the implementation or expected prose. Promote only durable project-wide rules into
AGENTS.md, and update a skill only when the reusable procedure changes.

## Durable guidance

AGENTS.md and the project skills are versioned with source and contain no private
workstation paths. Keep local tools, presets and lessons in a separate private,
versioned backup. Record hashes and restore locations; an ignored folder on the
same workstation is not a remote backup. Do not publish vendor binaries, raw logs,
private captures or the entire research archive with a documentation update.
