# ReShade integration

TRP runs ReShade effects once per source frame, before frame generation, and
keeps the ReShade overlay at native resolution. Effects can run before or after
upscaling; **after upscaling is the default**.

## Setup and settings

Use your existing ReShade installation, `ReShade.ini`, preset and shader paths.
TRP does not include ReShade or change its hotkeys. Leave **SSE ReShade Helper
disabled**; TRP supplies the effect and overlay stages.

To change placement, use **Advanced -> ReShade before upscaling**, or save:

```ini
[Compatibility]
ReShadeBeforeUpscaling=false
```

Switching placement may reload shaders. Before-upscaling effects use the active
render resolution; after-upscaling effects use output resolution. Community
Shaders supplies unfinished HDR color before upscaling, so presets can look
different there. Start with the default placement.

The `DEPTH` input uses Skyrim's raw reversed-Z depth at the effect resolution.
Presets must interpret that depth correctly. Menu frames receive far-plane depth.
Effects-off preserves the overlay and input handling.

## Compatibility

The ReShade integration was tested with ReShade 6.3.3.1921, Skyrim 1.6.1170,
Cabbage ENB and an RTX 4080 SUPER: gameplay, x4 frame generation, NR before
upscaling, effect toggling and movement after closing the overlay worked.
This does not establish a measured performance gain or validate other changes
combined into a later build.

Before-upscaling effects, NR after upscaling, depth-dependent preset appearance,
Community Shaders, transitions and other hardware still need game validation.
NR settings and edition-specific frame-generation capabilities are preserved.

## Integration and tests

One D3D11 runtime handles effects and input; the D3D12 presentation chain has
no second effect runtime. Effects finish before FG scene capture. Existing NR
ordering is preserved: late NR follows late ReShade on the TRP/ENB path, while
late ReShade follows late NR on the CS path. Context state and resource retirement
are preserved. The bundled public API headers are pinned to ReShade 6.3.3/API 14.

CTest includes `ReShadeAbsent` and frame-order checks. To exercise the actual
runtime, build `TRPReShadeIntegrationTests`, place it with
`tests/fixtures/reshade/` in an ignored directory, and supply your ReShade DLL
as `dxgi.dll`. Run `./TRPReShadeIntegrationTests.exe --require-reshade` there.
It checks pixels, depth extents, placement changes, input, output-runtime
isolation and recreation. Restore the fixture INI before repeating the test.

For device-ownership regressions, run `TRPSourceDLSSGInteropFenceTests.exe
--require-wrapped` and `TRPD3D11FrameCopyTests.exe --require-wrapped` beside that
DLL. These checks reject foreign devices and verify fence waits, repeated depth
copies and restored state. They do not launch Skyrim or run NVIDIA inference.
