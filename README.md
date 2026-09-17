# Theo's Render Pipeline

NVIDIA rendering integration for Skyrim: DLSS/DLAA, multi-frame generation,
optional Neural Rendering and native-resolution menus and HUD. NVIDIA runtimes
provide inference and generated-frame presentation.

This source archive builds `TheosRenderPipeline.dll`, including external ImGui integration.
Install packages are supplied separately and link their matching Git revision.
The full-feature package contains NR integration and the Ada MFG unlock, with
all NVIDIA runtimes supplied separately. It needs no other renderer package.
See the installation guide for exact runtime download paths.

## Features

- DLSS Super Resolution, DLAA, model presets and sharpening.
- Frame generation and native MFG capabilities. The full-feature build selects
  its unlock using the rendering GPU; the existing Ada opt-out remains available.
- Full-feature build: NR before or after DLSS, one or two passes, input scaling and tuning.
- Spatial scaling for loading-screen backgrounds and an optional request for
  transition artwork, enabled by default under Advanced. Skyrim chooses the art.
- Native UI composition, inventory/spell previews, startup overlays, external
  ImGui integration, HDR output conversion and GPU measurements.

**End** opens settings. **Apply now** changes the session; **Save as default**
persists settings; **Discard changes** drops unapplied edits. NR starts off.
The NVIDIA host remains required when interpolation is off.

## Build and install

See [build instructions](docs/BUILD.md), [architecture](docs/ARCHITECTURE.md) and
[installation and controls](package/README.md). Vendor runtimes are not included
in Git.

The plugin and SKSE identity are `TheosRenderPipeline`. Published `SolFG_*`
companion exports retain their names and layouts in `TheosRenderPipeline.dll`.

The tested environment is Skyrim 1.6.1170 with matching SKSE64/Address Library,
LoreRim/ENB and RTX 4080 SUPER. The rc.2 menu build received a positive gameplay
report with Before-DLSS NR/one pass and x4 MFG active. World, DLSS and Output
open Image settings; NR and Frame generation open their respective tabs.
The same DLL supports Skyrim 1.5.97 and 1.6.1170 and selects the matching engine
hooks and layouts at startup. The universal build needs gameplay testing on both
versions; 1.5.97 support is experimental. Native RTX 50-series, HDR and physical
frame cadence need separate testing.

Use SKSE64 and Address Library matching your Skyrim executable. Full and Standard
remain separate feature editions; each supports both game versions.

ENB is not a startup requirement; setups without ENB have not been validated.

See [LICENSE](LICENSE) and [third-party notices](THIRD-PARTY.md) for project and
contribution terms. Vendor runtimes retain their own licenses.
