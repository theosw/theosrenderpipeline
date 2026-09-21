# NR bottleneck reuse feasibility probe

Standalone research only. This directory is not part of the plugin build,
installed package, settings or menu. It cannot launch or attach to Skyrim.
The comparison script refuses to measure while SkyrimSE is running.

The experiment leaves NR's encoder and decoder active on every frame but omits
the coarsest transformer block on alternate frames, retaining its last output.
It investigates the technique published by
[janblade's OptiScaler fork, v0.1.13](https://github.com/janblade/OptiScaler-DLSSNR-PreSR-Multipass/releases/tag/v0.1.13-dlssnr-vit-reuse),
specifically [34e33c6](https://github.com/janblade/OptiScaler-DLSSNR-PreSR-Multipass/commit/34e33c6a2a1083b01a7d12807f8e0703e64f8dff).
Credit goes to that project for the reuse technique. This probe's observer,
strict scheduling checks and comparison harness are implemented separately;
it does not import OptiScaler's hybrid model or launch filter.

The minimal experimental NvAPI ABI declarations correspond to
[NVIDIA/nvapi 87dca625, nvapi.h](https://github.com/NVIDIA/nvapi/blob/87dca625e83fd89a983e19b904e5f3a580da90d2/nvapi.h).
They are private/experimental interfaces, not a supported NVIDIA NR option.
No runtime, weights or device program is included. Supply the existing NR
310.8 DLL, SHA256
`8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206`.
Other NR DLLs are rejected by the observer.

## Build and compare

Use an independent checkout and build under `out/`. Requires Windows, MSVC,
CMake, the local NGX SDK, and Python with NumPy. No deployment is involved.

```powershell
cmake -S research/nr-bottleneck -B out/build/bottleneck -G "Visual Studio 17 2022" -A x64 "-DNGX_SDK=<Streamline external/ngx-sdk>"
cmake --build out/build/bottleneck --config Release --parallel 3
ctest --test-dir out/build/bottleneck -C Release --output-on-failure
python research/nr-bottleneck/compare.py --exe out/build/bottleneck/Release/TRPNRBottleneck.exe --dll <local-nvngx_dlssnr.dll> --output out/runs/bottleneck-720 --width 1280 --height 720 --frames 32 --reset-every 16
```

The script creates deterministic gradients/edges, pixel-motion vectors, flat
depth, and a scene/lighting change aligned with a reset. It runs unmodified NR,
observation, reuse and a second unmodified reference in separate processes.
The executable also accepts `baseline`, `observe` or explicit `reuse` mode for
an existing RGBA16F/RG16F input sequence; run without arguments for the syntax.

Evidence stays in the new output directory: runtime/executable/input hashes,
child exit status, logs, per-frame readbacks, kernel argument traces and timing
CSV. The script refuses nonzero exits, timeouts, invalid images, changed alpha,
pass-through pixel changes or resets that fail to reproduce reference pixels.
Do not upload raw traces or private runtimes. `RESULTS.md` records reviewed
aggregate evidence and limits.

## What the guards establish

- The observer wraps only the verified NR DLL's `GetProcAddress` import in this
  private process, then two NvAPI query results. It does not globally detour the
  driver, spoof a GPU, change DLSS/FG ownership or hook the game.
- Warmup checks the exact 42-launch FP8 bottleneck and its immediate encoder/
  decoder boundaries. Later frames must retain launch names, shapes and sizes;
  skipped parameters must match the last completed full evaluation.
- The repack result must be the next decoder's input. An argument scan rejects
  additional exact output-address references outside that block on every frame.
  This does **not** prove all kernel reads/writes, interior aliases or counters.
- Any recording anomaly prevents command-list submission. This is intentionally
  a terminal research error; it is not a game-ready fallback after partial work.
- Only the successful completion fence admits cached data. Each plan belongs to
  one feature generation, and reset forces full computation. The probe runs one
  pass, preset 0, fixed tuning and fixed dimensions per process.
- Callback state and explicitly loaded modules remain alive through process
  detach because the runtime can cache function pointers. Retention is bounded
  to one short-lived observer; it is not a production lifetime solution.

CPU tests exercise alternation, reset/toggles, retirement, generation replacement,
missing/extra/foreign launches, changed shapes/bindings and output alias rejection.
They run in Release without relying on disabled `assert` calls.

## Before renderer integration

Measure broader motion and lighting sequences, then verify counter and memory
dependencies at other extents, model presets, passes and GPU architectures.
Design generation-based retirement and fallback for the real NR owner, including
loading/tuning/placement changes and existing CS/NvAPI wrappers. An offline
completion wait does not validate production pipelining. Do not transplant this
observer into the plugin or claim smooth gameplay from average GPU milliseconds.
The previous whole-evaluation reuse experiment caused user-reported microstutters;
partial reuse still needs its own cadence and visual acceptance.
