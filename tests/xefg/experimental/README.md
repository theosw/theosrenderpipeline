# Isolated XeFG x2–x6 experiment

The fixture and the opt-in renderer now share `src/FrameGen/XeFGUnlock.cpp`.
The renderer defaults to official x2; its experimental checkbox enables x3-x6
after checked admission and presenter recreation. Fixture-only pixel capture,
event storage and fault injection are compiled out of the renderer. Building
or running these fixtures does not deploy anything or change a runtime file.

The experiment accepts exactly Intel `libxess_fg.dll` 1.3.1.78, SHA256
`ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27`.
It checks PE identity and the whole mapped `.text` against the pinned file,
including ASLR relocation, then installs five gate contracts and three pacing
detours as one checked transaction before context entry. The ceiling is
five generated images (x6 total). Native forwarding is established before
publication. A verified rollback returns `Refused`; uncertain rollback returns
`Unsafe` and cannot be treated as a healthy official-x2 fallback.

Exactly one SDK owner is permitted. Call `NewContextEpoch` with no live provider
work before each reinitialization. The patched runtime and hook code remain
loaded until every SDK context is destroyed. Context/epoch changes invalidate
thread-local history. The fixture intentionally exits on any failed retirement.

After building `tests/xefg`, run the CPU transaction test and production interop
test with CTest. GPU execution requires an explicit runtime directory:

```powershell
TRPXeFGPrototype.exe --runtime-dir <sdk/bin> --debug-layer --frames 96
TRPXeFGPrototype.exe --runtime-dir <sdk/bin> --debug-layer --frames 96 --unlock --multiplier 3 --events x3.csv
TRPXeFGPrototype.exe --runtime-dir <sdk/bin> --debug-layer --frames 96 --unlock --multiplier 4 --events x4.csv
TRPXeFGPrototype.exe --runtime-dir <sdk/bin> --debug-layer --frames 120 --unlock --cycle-multipliers --events cycle.csv
TRPXeFGPrototype.exe --runtime-dir <sdk/bin> --debug-layer --frames 96 --unlock --multiplier 4 --capture-dir pixels
python experimental/analyze.py --pixels pixels --multiplier 4 --output evidence.json
TRPXeFGPrototype.exe --runtime-dir <sdk/bin> --debug-layer --frames 120 --unlock --multiplier 6 --capture-dir pixels-x6
python experimental/analyze.py --pixels pixels-x6 --multiplier 6 --output evidence-x6.json
TRPXeFGPrototype.exe --runtime-dir <sdk/bin> --debug-layer --frames 96 --unlock --multiplier 4 --force-scheduler-refusal --events refusal.csv
python experimental/analyze.py --events refusal.csv --require-even-pacing --output refusal.json
```

The fixture recreates at two dimensions and depth conventions, toggles FG,
and resets history. `--minimum-interval-us 0` removes the XeLL cap; the default
16667-us cap produces approximately 60 output deliveries/s. Input GPU-copy
retirement and SDK-owned presentation retirement remain separate boundaries.

Pixel readback is bounded to three selected-multiplier bursts, before native Present, on the
provider's queue after its output copy. It restores the backbuffer to PRESENT
and waits on a completion fence before releasing capture resources. Capture
perturbs pacing: analyze timings from an independent run without capture.
The central image hashes exclude corner debug tags and HUD. Different hashes
establish distinct generated scene pixels, not good reconstruction quality.
The synthetic checkerboard shows substantial artifacts.

Hook QPC gaps describe software delivery, not scanout. This does not establish
AMD acceptance, Skyrim integration, latency, display cadence, sustained real
workload stability or interpolation quality. `TRPXeFGPresenterTests` separately
checks production-owner and optional NVIDIA-provider handoff. Set
`TRP_XEFG_MFG_TEST=1` for live x2 through x6, recreation and disable-to-x2 coverage;
with `TRP_XEFG_EXPECT_REFUSAL=1`, it instead requires x2 fallback using a
separately identified unsupported runtime copy. The
refusal fixture passes index zero to intermediate scheduler calls, exercising
the actual SDK's early false return before any wait/history update. A zero
gate did not refuse scheduling. This injection exposed a compressed last-frame
slot; the corrected fallback derives every slot from burst index and also
paces the final image when earlier calls refused. The old trace fails the
`--require-even-pacing` check; the corrected trace passes. That check compares
p25/p95 software delivery gaps and is only a coarse fixture regression check.
Natural history refusals and the entirely disabled scheduler mode remain separate.

Patch/pacing facts are adapted from Coldwood1026's GPL-3.0
OptiScalerDp4aUnlock at `9eea95bba9fda7121f214d2eba358423be598d7e`.
Those portions retain GPL-3.0 without TRP's additional exceptions. No PureDark
implementation is copied. No private runtime binaries are included.
