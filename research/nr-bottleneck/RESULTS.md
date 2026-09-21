# Initial bottleneck reuse results

September 21, 2026. Draft feasibility evidence, not release/game acceptance.
Base: TRP `release/0.2.3`, `6011df5ca7fe50a8dc638d1c9fed1d757cd6887e`.
The unchanged production FeatureSession is compiled into a standalone executable;
the instrumentation and reuse policy are confined to that executable.

## Final executable and experiment

- RTX 4080 SUPER, driver 591.86. Skyrim absent before and after the measurements.
- NR 310.8 SHA256:
  `8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206`.
- Final probe SHA256:
  `0FE0CFD2234FFAE407721F90820D15E97C8A1786DF218D2206DC8AC77EC73C05`.
- Final bundle: local ignored `out/runs/final-pan-720`; its `.run.json` files pin
  the executable, runtime and shared input manifest and retain successful exit status.
- 1280x720 synthetic moving gradients/edges, 32 frames, two-pixel horizontal
  motion per frame, depth 0.5, no jitter/UI, model preset 0, one pass, style 0,
  intensity/tone/structure 1, skin -1. Frame 16 changes colour/lighting and resets.
- Four isolated children: reference, observed reference, reuse, repeated reference.
  All completed readback, feature release and NGX shutdown with exit 0.

## Results

Two initial frames (one full/reuse pair) are excluded from timing summaries.
The reuse summary therefore contains 15 full and 15 reused evaluations.

| Mode | Mean NR GPU ms | p95 ms | Standard deviation ms |
| --- | ---: | ---: | ---: |
| Reference | 3.1155 | 3.3452 | 0.1338 |
| Observed reference | 3.0781 | 3.0950 | 0.0126 |
| Bottleneck reuse | 2.6873 | 3.0910 | 0.3935 |
| Repeated reference | 3.0769 | 3.1025 | 0.0140 |

The stable observed/repeated controls put the average saving around **12.7% of
NR Evaluate GPU time** (about 0.39 ms here). Reuse still alternates approximately
**3.081 ms full / 2.294 ms reused** work. It does not reduce the full-evaluation
cost or establish improved display cadence. Initial-reference and preceding-run
outliers show why these short samples are not a universal benchmark. The earlier
720p prototype averaged 2.743 ms versus 3.078 ms (~10.9%), with p95 3.409 ms.
That older executable lacks the final per-frame consumer/alias recheck and is
not the final binary's result.

Both observed and repeated references reproduce every reference output exactly:
**64/64 full-frame matches**. Reuse has finite output and identical alpha on all
32 frames. Its reset frames (0 and 16) exactly reproduce the reference. Across
all frames/pixels/channels, mean absolute RGB difference is **0.001551**, with
maximum individual channel difference **0.212646** on a nominal 0–1 input scale.
The average is small but the localized maximum is substantial; no perceptual
acceptance is claimed. Computed frames after reuse may also differ because NR
consumes temporal history. Only reset frames are expected to restore exactness.

The trace records 156 launch requests per frame. Reuse omits the 42 bottleneck
launches on 16 frames and none on the other 16 (including warmup). Their arguments
match the last completed full evaluation. The repack output is observed as the
next decoder's input; the argument scan finds no additional exact pointer aliases
outside the range. This is not a proof of all device-code/counter dependencies.

## Validation and boundaries

- Final Release build and CTest pass. The CPU suite checks alternation, live
  enable/disable, resets, completion admission, generation replacement, incomplete/
  extra/foreign launches, changed shape/parameter/consumer bindings and aliases.
- An earlier pass-through probe matched all eight pre-existing 320x320 oracle
  outputs but returned exit 1 after reporting successful shutdown. Its observer
  state was destroyed while cached callbacks could survive. Retaining the callback
  state through process detach corrects the lifecycle defect; subsequent runs exit
  cleanly. The failed process remains excluded from accepted measurements.
- The initial sandboxed compiler configuration could not read the Windows SDK
  cache. Builds subsequently used normal host SDK access. No build deployed files.
- No production source, CMake wiring, default, menu or installed package changed.
  No game was launched. No runtime, captured image, weight or raw trace is published.

Unverified: perceptual quality in Skyrim, microstutters/scanout, real guide formats,
CS/ENB integration, multiple in-flight frames, multipass/preset changes, complete
GPU memory/counter dependency proof, feature recreation in one live process,
other dimensions/models/architectures, and RTX 30 performance. Keep this as a
research draft; do not merge it as an enabled renderer optimization.
