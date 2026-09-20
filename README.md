# Theo's Render Pipeline

Version **0.2.0** adds ReShade effects and overlay support, fixes startup without
ReShade, and corrects RTX 30 runtime loading under MO2 and Community Shaders.
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
- Both editions: NR before or after DLSS, one or two passes, input scaling and tuning.
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

Full selects experimental Ampere compatibility on RTX 30-series, the Ada MFG
unlock on RTX 40-series, and native capabilities on RTX 50-series. Standard
includes NR and excludes both compatibility paths; RTX 40-series uses native x2.

**Skyrim 1.5.97, 1.6.640 and 1.7.104 are experimental and untested in-game.**
RTX 30-series execution, native RTX 50-series operation and HDR appearance remain
unverified. Both 0.1.4 editions received positive Skyrim 1.6.1170/RTX 4080 SUPER
reports with Cabbage ENB and Bottle's Community Shaders build: native x2 in
Standard, Ada x4 in Full, and both NR placements in each setup. Full also logged
x6 in the CS run. Recurring Streamline RSYNC errors remain recorded in some
runs; no physical frame-cadence claim is made. Individual UI/transition checks
and compatibility with other CS builds are not inferred from overall feedback.

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

## Public ZIP installer metadata

The public Full download is named **Universal**. MO2 should suggest distinct,
version-independent names: **Theo's Render Pipeline - Standard** and
**Theo's Render Pipeline - Universal**. Install Standard first, then Universal
as a separate mod below it in the left pane if those features are wanted.
Enable both and let Universal win conflicts; do not merge the editions.
Universal can also use separately supplied NVIDIA runtimes.

After assembling each public package, before creating its ZIP, run:

```powershell
pwsh -File tools/Set-PackageInstaller.ps1 -PackageDirectory <public-package-folder> -Edition Standard -Version 0.2.0
pwsh -File tools/Set-PackageInstaller.ps1 -PackageDirectory <public-package-folder> -Edition Universal -Version 0.2.0
```

Use the corresponding folder for each command. This adds `fomod/info.xml` and
`fomod/ModuleConfig.xml` with one required installation page. Older FOMOD Plus
versions crash when an installer has no pages. Every public payload file and notice
is installed in its existing location; no renderer rebuild or filename change
is needed. Internal build names and `TRP-FULL-PACKAGE.txt` remain unchanged.
Apply this only to public staging folders, after private manifests and symbols
have been excluded. The script refuses private staging trees. MO2 users can
still override the proposed mod name. FOMOD Plus versions may ignore the name in
`info.xml`, so also put the edition before the version in archive filenames:
`Theos Render Pipeline Standard-0.2.0.zip` and
`Theos Render Pipeline Universal-0.2.0.zip`. MO2 2.5.2's local-file fallback stops
at punctuation or digits, so keep the leading name in letters and spaces.
FOMOD Plus may therefore show the name without the apostrophe and hyphen.
Keep Nexus download names
edition-specific too; installer plugins may prefer that metadata.
