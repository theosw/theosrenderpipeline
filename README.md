# Theo's Render Pipeline

Version **0.2.2** enables Neural Rendering with native DLAA, before or after
anti-aliasing, and improves experimental RTX 30 compatibility. Missing
compatibility keys now use the packaged default; explicit opt-outs remain
respected. Startup diagnostics identify the effective renderer and settings.
RTX 3060 Laptop testing with Community Shaders confirms frame generation and
NR execution. The 0.2.1 Ultra Quality fix and ReShade/CS support are retained.
CS keeps shading, upscaling and UI; TRP supplies
frame generation, Reflex and optional world-only NR. Disable CS frame generation
and Reflex when using TRP. See [setup and validation limits](docs/COMMUNITY_SHADERS.md).

NVIDIA rendering integration for Skyrim: DLSS/DLAA, multi-frame generation,
optional Neural Rendering and native-resolution menus and HUD. NVIDIA runtimes
provide inference and generated-frame presentation.

This source archive builds `TheosRenderPipeline.dll`, including external ImGui integration.
Install packages are supplied separately and link their matching Git revision.
The full-feature package contains NR integration and Ada/Ampere MFG compatibility, with
all NVIDIA runtimes supplied separately. It needs no other renderer package.
Standard includes the SR/FG and NR runtimes, with NR off by default. Full can
also use Standard's runtimes when installed after it in MO2.
See the installation guides for each edition.

## Features

[ReShade integration](docs/RESHADE.md) adds source-frame effects, explicit depth
and a before/after-upscaling choice. Keep SSE ReShade Helper disabled.
Full has positive ENB gameplay reports with and without ReShade; see the guide
for tested configurations and remaining limits.

- DLSS Super Resolution, DLAA, model presets and sharpening.
- Frame generation and native MFG capabilities. The full-feature build selects
  its compatibility path using the rendering GPU; the existing opt-out remains available.
- Both editions: NR before or after DLSS/DLAA, one or two passes, input scaling and tuning.
- Spatial scaling for loading-screen backgrounds and an optional request for
  transition artwork, enabled by default under Advanced. Skyrim chooses the art.
- Native UI composition, inventory/spell previews, startup overlays, external
  ImGui integration and GPU measurements. HDR is not supported in this release.

**End** opens settings. **Apply now** changes the session; **Save as default**
persists settings; **Discard changes** drops unapplied edits. NR starts off.
The NVIDIA host remains required when interpolation is off.

## Build and install

See [build instructions](docs/BUILD.md), [architecture](docs/ARCHITECTURE.md) and
[installation and controls](package/README.md). Vendor runtimes are not included
in Git.

The plugin and SKSE identity are `TheosRenderPipeline`. Published `SolFG_*`
companion exports retain their names and layouts in `TheosRenderPipeline.dll`.

Full selects experimental Ampere compatibility on RTX 30-series, the Ada MFG
unlock on RTX 40-series, and native capabilities on RTX 50-series. Standard
includes NR and excludes both compatibility paths; RTX 40-series uses native x2.

**Skyrim 1.5.97, 1.6.640 and 1.7.104 remain experimental.** The Nolvus Awakening
6.0.20 Universal input candidate has scoped positive F10, x5, both NR placements
and Wheeler feedback on 1.5.97. See [Nolvus setup and test limits](docs/NOLVUS.md).
Skyrim 1.6.640 and 1.7.104 remain untested in-game.
RTX 30 compatibility remains experimental: a 3060 Laptop/CS volunteer run records
x2/x3/x4 outputs and NR inference, with grass-edge artifacting and occasional
hitches still reported. Native RTX 50-series operation remains unverified.
Both 0.1.4 editions received positive Skyrim 1.6.1170/RTX 4080 SUPER
reports with Cabbage ENB and Bottle's Community Shaders build: native x2 in
Standard, Ada x4 in Full, and both NR placements in each setup. Full also logged
x6 in the CS run. Recurring Streamline RSYNC errors remain recorded in some
runs; no physical frame-cadence claim is made. Individual UI/transition checks
and compatibility with other CS builds are not inferred from overall feedback.
The 0.2.2 DLAA/NR correction has positive ENB/RTX 4080 SUPER feedback at
5120x1440 with x4 and NR before and after DLAA. This does not establish DLAA/NR
acceptance on RTX 30 or every CS build. See [RTX 30 setup](docs/RTX30_TESTING.md).

See the [1.7.104 port notes](docs/SKYRIM_1_7_104.md) and
[1.6.640 port notes](docs/SKYRIM_1_6_640.md). World, DLSS and Output open Image
settings; NR and Frame generation open their respective tabs.

Use SKSE64 and Address Library matching your Skyrim executable. Full and Standard
remain separate feature editions; each supports all four game versions.

The same DLL supports ENB and Community Shaders installations. It selects the CS
path when CommunityShaders.dll is loaded; otherwise TRP owns upscaling. ENB is
optional. Use one shading setup per profile. CS setup is documented above.

See [LICENSE](LICENSE) and [third-party notices](THIRD-PARTY.md) for project and
contribution terms. Vendor runtimes retain their own licenses.
