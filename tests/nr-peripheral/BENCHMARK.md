# Manual NR benchmark

Build with `TRP_NR_VENDOR_BENCHMARK=ON` and a local `NGX_SDK` path. The
benchmark is opt-in and is never run by CTest or the build.

```
TRPNeuralPeripheralBenchmark.exe absolute-NR-DLL native width height 0 final.f4
TRPNeuralPeripheralBenchmark.exe absolute-NR-DLL native width height 0 final.f4 --frames measured.f4
```

The first command preserves the plain timing sequence: 10 warm-up evaluations,
40 measured evaluations, then a final corrected float4 readback. Only this mode
reports `NR_total_ms`. Run the second command as a separate quality measurement.
It uses the same input, reset on frame 0, and 50-evaluation seed/history sequence,
but reads corrected output after each frame 10 through 49. Its `QUALITY` record
explicitly marks it untimed; it reports no `RESULT` or timing `SAMPLE` records.
Both commands can write the existing frame-49 file.

`measured.f4` contains 40 consecutive images, in frame order 10..49. Each image
contains width*height little-endian RGBA float32 pixels, row-major, without a
header. Compare RGB only; pool all squared errors before converting to PSNR
using peak 1. Do not average frame PSNRs or images. Use a same-size reference
with the same runtime, preset, input, reset, seed, and frame sequence. Also
report the worst frame, maximum absolute RGB error, and RGB absolute-error
99.9th percentile over the full measured sequence. Keep final-frame PSNR as a
diagnostic. Verify plain/export final frames match before trusting a new setup.

`--input input.f4` replaces the synthetic checkerboard with exactly width*height
finite RGBA float32 pixels. The image is uploaded using the existing format;
motion remains zero and depth 0.5. Custom input uses finite, complete output and
nonblack-output-for-nonblack-input validation. The default synthetic input retains
its changed-component assertion. Every exported measured frame is validated.
Photographs with static depth/motion do not qualify game temporal behavior.

`--skip-cleanup` is an explicit research-only workaround for vendor runtime
shutdown failures/hangs after completed measurements. It prints a cleanup-skipped
record, flushes output and exits without feature/NGX teardown. Default cleanup
is unchanged. A successful run with this flag does not qualify shutdown.
