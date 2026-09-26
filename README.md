# Theo's Render Pipeline

NVIDIA rendering integration for Skyrim: DLSS/DLAA, frame generation, optional
Neural Rendering (NR), and native-resolution menus and HUD. Current version:
**0.3.0**. See [CHANGELOG.md](CHANGELOG.md) for release changes.

## Features

- DLSS Super Resolution, DLAA, model presets and sharpening.
- Frame generation and multi-frame generation (MFG).
- NR before or after DLSS/DLAA, one or two independently configured passes,
  input scaling and tuning, in both editions. NR defaults off.
- Optional peripheral compression and combined NR preparation, both off by default.
- ReShade source-frame effects, explicit depth and placement controls.
- Native UI composition, inventory/spell previews, loading artwork and external
  ImGui integration, with live GPU measurements beside the settings.

**End** opens settings. **Apply** changes the session; **Save as default** persists
settings and window layout; **Discard** drops unapplied edits. NR keyboard
shortcuts are opt-in. HDR is unsupported.

With two NR passes selected, optional **One pass in combat** and **One pass while
weapons/spells are drawn** controls temporarily skip the second pass. The return
delay defaults to five seconds after all selected conditions clear and pauses
with the game. Your saved pass count, resolution and tuning remain unchanged.
The second pass stays allocated and its history resets when it resumes. These
options default off; their effect on responsiveness depends on the workload.

## Install

Use SKSE64 and Address Library matching your Skyrim executable. Install packages
are supplied separately; this repository contains source, not the NVIDIA runtime
DLLs. Follow the [Standard installation guide](package/STANDARD-README.md) or
[Universal installation guide](package/README.md).

Standard includes the SR/FG and NR runtime bundle. Universal adds the compatibility
paths and can use Standard's runtimes when installed after it in MO2, or separately
supplied matching runtimes. Both editions retain NR. Enable only one winning
renderer DLL. The NVIDIA host is required even when interpolation is off.

The DLL selects Community Shaders integration when CommunityShaders.dll is loaded;
otherwise TRP owns upscaling, with optional ENB. With CS, disable its frame
generation, Reflex and HDR; CS retains its shading, upscaling and UI. Keep SSE
ReShade Helper disabled when using TRP's ReShade integration. Use one shading
setup per profile and disable competing upscaler/frame-generation injectors.

## Compatibility

| Edition | Frame-generation capabilities |
| --- | --- |
| Standard | Native NVIDIA capabilities; RTX 40 uses native x2 |
| Universal | Experimental RTX 20/30 compatibility, RTX 40 MFG unlock, native RTX 50 capabilities |

The loader supports Steam Skyrim **1.5.97, 1.6.640, 1.6.1170 and 1.7.104**.
Versions 1.5.97, 1.6.640 and 1.7.104 remain experimental; 1.6.640 and 1.7.104
are untested in-game. Nolvus Awakening 6.0.20 has scoped positive Universal
input/x5/NR feedback on 1.5.97. The plugin/SKSE identity is `TheosRenderPipeline`;
existing `SolFG_*` companion exports retain their names and layouts.

RTX 20 evidence is limited to an RTX 2060 volunteer run reporting x2/x3/x4/x6,
NR and loading recovery; weapon jitter remains unresolved. GTX 16 is excluded.
See [RTX 20 setup and test limits](package/RTX20-TEST.md). RTX 30 evidence includes
a 3060 Laptop/CS run with x2/x3/x4 and NR, plus reported grass-edge artifacts and
occasional hitches. Native RTX 50 operation remains unverified.

The combined 0.3.0 release has positive ENB/RTX 4080 SUPER feedback with verified
x4/NR and loading recovery. Earlier runs cover Standard native x2, Universal MFG,
ReShade/CS integration and DLAA with NR before/after at 5120x1440. These results
do not establish every GPU, modlist or checkbox. Recurring vendor RSYNC errors
remain recorded in some runs; generated-FPS counters do not establish physical
display cadence. DLAA/NR on RTX 30 and every CS build remain unverified.

## Build

Requires Windows, Visual Studio 2022 C++ tools/Windows SDK, CMake and vcpkg.
Use CommonLibSSE-NG 8.1.0 at `3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd`,
Streamline 2.11.1 public headers and NGX SDK headers/import library, and
Nukem9/detours at `cc5a2e4a58ef462821877b35ad30215f0e16bba1`. CommonLib's
patch diagnostics fetch HDE64 from MinHook v1.3.4. Other libraries are listed
in `vcpkg.json`; runtime DLLs are not needed to compile.

Place supplied dependencies under ignored `.dependencies/`, and adjust these
example paths to your installations:

```powershell
cmake --preset arp-nvidia `
  "-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  "-DTRP_COMMONLIBSSE_NG_DIR=C:/deps/CommonLibSSE-NG" `
  "-DTRP_NGX_SDK_DIR=C:/deps/Streamline/external/ngx-sdk" `
  "-DTRP_STREAMLINE_INCLUDE_DIR=C:/deps/Streamline/include" `
  "-DARP_DETOURS_DIR=C:/deps/detours"
cmake --build --preset arp-nvidia --parallel 2
```

`ARPBaseline` builds the renderer in `out/build/nvidia/Release` without deploying
or launching. The build compiles and embeds NR shader bytecode; generated headers
stay in the build directory. Configure separate directories for Standard
(`TRP_ENABLE_OPTIONAL_FEATURES=OFF`) and Universal (`ON`, default), keeping
`TRP_ENABLE_NEURAL_RENDERING=ON` for both. Store workstation paths in ignored
`CMakeUserPresets.json`. Set `TRP_NGX_LIB` if the NGX import library is elsewhere.

Enable `TRP_BUILD_COMPATIBILITY_TESTS=ON` for the standalone checks, build all
targets with `cmake --build <build-directory> --config Release`, then run
`ctest --test-dir <build-directory> -C Release --output-on-failure`. These checks
do not establish game acceptance. Build output alone is not a complete install;
use the installation guides for configuration, shaders and required runtimes.

See [LICENSE](LICENSE) and [third-party notices](THIRD-PARTY.md) for project,
dependency and contribution terms. Vendor runtimes retain their own licenses.
