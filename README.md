# Theo's Render Pipeline

NVIDIA rendering integration for Skyrim: DLSS/DLAA, multi-frame generation,
optional Neural Rendering and native-resolution menus and HUD. NVIDIA runtimes
provide inference and generated-frame presentation.

This source archive builds `TheosRenderPipeline.dll`, including external ImGui integration.
Install packages are supplied separately and link their matching Git revision.
The full-feature package contains NR integration and Ada/Ampere MFG compatibility, with
all NVIDIA runtimes supplied separately. It needs no other renderer package.
See the installation guide for exact runtime download paths.

## Features

- DLSS Super Resolution, DLAA, model presets and sharpening.
- Frame generation and native MFG capabilities. The full-feature build selects
  its compatibility path using the rendering GPU; the existing opt-out remains available.
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

Version **0.1.3** combines experimental RTX 30-series (SM86) compatibility
with support for four exact Steam Skyrim versions. Full selects the Ampere
compatibility path on RTX 30-series, the Ada unlock on RTX 40-series, and native
capabilities on RTX 50-series. Standard excludes NR and both compatibility paths.

**Skyrim 1.5.97, 1.6.640 and 1.7.104 are experimental and untested in-game.**
RTX 30-series execution is also experimental and unverified. Both component
candidates received scoped Skyrim 1.6.1170/LoreRim/ENB regression tests on an
RTX 4080 SUPER, including Full x4/NR and Standard End closure during editing.
The final combined build has not had a separate game run. Native RTX 50-series,
HDR appearance and physical frame cadence remain unverified.

See the [1.7.104 port notes](docs/SKYRIM_1_7_104.md) and
[1.6.640 port notes](docs/SKYRIM_1_6_640.md). World, DLSS and Output open Image
settings; NR and Frame generation open their respective tabs.

Use SKSE64 and Address Library matching your Skyrim executable. Full and Standard
remain separate feature editions; each supports all four game versions.

ENB is not a startup requirement; setups without ENB have not been validated.

See [LICENSE](LICENSE) and [third-party notices](THIRD-PARTY.md) for project and
contribution terms. Vendor runtimes retain their own licenses.
