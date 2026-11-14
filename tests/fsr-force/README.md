# Standalone FSR 4 INT8 admission experiment

This is a Universal-only experiment for the exact official SDK 2.4 loader and
4.1.2.4555 upscaler. It does not change the renderer, menus, settings, packaging,
adapter identity, feature queries, presets, or driver selection. The compatibility
module is not yet wired into the production plugin. No AMD DLL is included.

Build separately (Windows x64, MSVC and Windows SDK):

```powershell
cmake -S tests/fsr-force -B D:/trp-build/fsr4-force/universal -G 'Visual Studio 17 2022' -A x64 -DTRP_ENABLE_OPTIONAL_FEATURES=ON
cmake --build D:/trp-build/fsr4-force/universal --config Release --parallel 4
```

Standard/OFF configuration is refused. A source-level compile guard also prevents
accidental compilation without the Universal experiment definition. The module
is independently written against Windows APIs and inspection of AMD's MIT-listed
runtime. No OptiScaler code or third-party modified provider is used.

Run one fresh child per case through `RunCase.ps1`. This workstation runner always
uses the required RTX 4080 GPU lock, checks the official files before and after,
retains a command/binary identity receipt, and enforces a 180-second external
process-tree timeout. The fixture has a 15-second fence wait. Do not execute the
fixture directly on the workstation. Do not remove a GPU-failure stop marker to
retry a crashed/hung provider.

```powershell
$fixture = 'D:/trp-build/fsr4-force/universal/Release/TRPFSRForceFixture.exe'
$evidence = '<absolute evidence directory>'
& tests/fsr-force/RunCase.ps1 -Executable $fixture -EvidenceRoot $evidence -Name force-proof-1440 -Mode force -Kind proof -Frames 24
```

The fixture accepts 1280x720 or 1920x1080 input and exactly twice those dimensions
as output. The synthetic scene, 32-phase Halton jitter, motion, forward depth,
RGBA16F linear colour and explicit unit exposure are identical for each method.
The descriptors and resource transitions follow `FSRUpscaler`; this direct D3D12
host excludes TRP's D3D11 bridge. NR, FG and sharpening are absent/disabled.

Modes are `auto`, `classic`, `force`, `bad-preimage`, and `preloaded`. A separate
bad-hash test supplies a directory containing a one-byte dummy loader; never
alter the official files. `bad-preimage` deliberately alters one instruction byte
in that child's memory, checks rejection before any query, and verifies that the
guard left the injected bytes untouched. These deliberate refusals return 20.

Run kinds:

- `timing`: no watermark or debug flags; 120 warmup frames then the requested
  measured count (default 240). Timestamp scope encloses only FFX dispatch GPU
  work, excluding scene production, copies, readback and the D3D11 bridge.
- `proof`: `MLSR-WATERMARK=1` in both CRT and Windows environments before loading;
  API checking enabled; eight warmup frames. Retains PNG and full FP16 readback.
- `debug`: proof plus the FFX debug view and D3D12 debug layer; drains its info
  queue to the log and fails on error/corruption messages. Never use for timing.
- `recreate`: two separately created/retired contexts and a mid-sequence history
  reset, with readbacks at resets and at the end of each generation.

The observer hashes successful compute PSOs (including recognized compute
pipeline streams), intercepts binding and direct dispatch, and writes execution
rows only for the observed submitted list after fence completion. Cached PSO
inputs and pipeline libraries are disabled. Unknown stream types, indirect work,
bundles, unknown PSOs or missing submission make observation unavailable, never
a success claim. It does not modify D3D12 feature queries. COM slots are checked
against the SDK C ABI at compile time. This diagnostic observer assumes the
fixture's one recording thread and one submitted list; it is not production
instrumentation. No shader bytecode or weight buffers are dumped.

The exact-build model observer inspects only allocation headers and specific
scalar fields: full INT8 provider vtable and ID, its model pointer/vtable, context
preset at `+0x208`, and lazy-construction flag at `+0x214`. The static model object
already exists at context creation; its context-specific GPU model/pipelines
are constructed on first dispatch. These observations require the pinned module
and do not generalize to another runtime.

Cross-check all trace rows offline against the research inventory:

```powershell
python tests/fsr-force/analyze.py $evidence '<absolute shader-inventory.json>'
```

`analysis.json` includes every executed shader's full SHA-256, entry, SM target,
instruction count, requirements, sequence/fence consistency, and timing tails.
It deliberately leaves watermark verification to image inspection. Neural
execution requires agreement between the actual provider/model, a complete
neural sequence including signed INT8 dot-product shaders, completed finite
output, and the visible INT8 watermark. Creation or a fast/slow timing alone is
insufficient. Image sanity here is not temporal/game quality acceptance.

The compatibility session must remain the sole SDK owner, with locked input
files held throughout use. It refuses preloaded AMD modules even when their
paths match. Its modules remain resident until process exit. Any uncertain patch
transaction poisons the session after attempting rollback; do not use its SDK
exports afterward. Returning to Automatic requires a fresh unpatched process.
