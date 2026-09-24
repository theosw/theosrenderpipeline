# Theo's Render Pipeline development

Read README.md and docs/BASELINE.md for current state, then docs/WORKFLOW.md for
the relevant work. Dated reports and archived Astra/SolFG checkouts are evidence,
not current source, deployment destinations or startup instructions.

- Use category/description branches: feature/, fix/, release/, diagnostics/,
  research/, perf/, test/, compat/, docs/ or chore/. Do not add codex/ or replace
  a category with an agent name unless the user explicitly requests it.
- Keep main current with reviewed releases. Verify whether an older PR's changes
  are already present before merging or closing it; ancestry alone is insufficient.
- Build and test under this checkout's out/. Concurrent tasks need independent
  checkouts/build directories. Never reuse another checkout's CMake cache. Keep
  configuration inputs frozen during a build; reconfigure after changing them.
- CMake never deploys. Assemble an identified package before deployment; preserve
  exact package identity, destination locking, rollback and live INIs. Use the
  current guarded helper when available. Confirm Skyrim and MO2 are closed before
  actual deployment or profile edits; do not interfere with another task's run.
- Launch only for a requested game test through the selected MO2 installation.
  Use the session's intended profile and destination, never a historical default.
  The user chooses a save. Do not automatically load a save or close their game.
  Reuse authorization already given; build/package requests alone do not authorize
  deployment, launch or upload.
- Source NVIDIA DLSS-G/MFG owns presentation; NvidiaHost owns shared host services.
  Preserve retirement-before-release, failure retention, plugin filenames, SKSE
  identity, public companion APIs and native UI behavior unless separately changed.
- MFG is required for release. Keep packaged compatibility enabled; distinguish
  native hardware, Ada unlock, Ampere and experimental Turing. x2 alone is not
  MFG acceptance. Preserve runtime admission, prerequisite and patch checks.
- Preserve NR in both editions, including its runtime, placement, passes and
  tuning. NR may default off. Obtain user agreement before reducing features or
  supplied runtimes; deferred runtime acquisition is not permission to omit them.
- Identify the loaded renderer and winning INI before attributing a regression.
  Separate standalone checks, actual runtime execution, user visual/input feedback
  and physical cadence. Preserve the configuration and limits of each claim.
- Keep .dependencies/, CMakeUserPresets.json, local/, out/, runtime DLLs, symbols
  and raw evidence out of public Git. Retain required notices and attribution.
- Before subsystem changes, read the matching guidance in docs/WORKFLOW.md and
  relevant lessons when available in local/knowledge/lessons/. Update a lesson
  after a meaningful fix, repeated failure or disproved hypothesis, with evidence
  and acceptance limits. Routine edits do not need a new lesson. Keep the current
  baseline concise; retain detailed chronology in dated reports.
- Use .agents/skills/arp-render-investigation for rendering/input diagnosis,
  arp-game-acceptance for requested game tests and trp-release-preparation for
  release/package work. Skills do not expand user scope or require permission again
  for an action already authorized. Ordinary edits need only relevant verification.
- Follow docs/BUILD.md and WORKFLOW.md; do not apply retired SolFG build or source
  invariant harnesses to this repository. Local tooling/configuration may be
  documented in local/README.md when present.
