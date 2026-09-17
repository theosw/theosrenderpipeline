# Building the plugin

Requires Windows, Visual Studio 2022 C++ tools/Windows SDK, CMake and vcpkg.
The build target and SKSE plugin identity are `TheosRenderPipeline`.

Supply CommonLibSSE-NG source, Streamline public headers and the NVIDIA NGX SDK
headers/import library. The validated dependency set uses CommonLibSSE-NG 3.6.0
and Streamline 2.11.1 headers. CMake reads the other libraries from `vcpkg.json`.
Runtime DLLs are not required to compile the plugin.

`arp-nvidia` builds one DLL for Skyrim 1.5.97 and 1.6.1170, with both CommonLib
SE and AE support enabled and VR disabled. The loader accepts only those two
versions. Runtime-aware addresses and layout accessors select the appropriate
engine integration. Use matching SKSE64 and Address Library files when installing.
Both Full and Standard use this universal configuration. The Full build has
been tested in-game on 1.6.1170. **1.5.97 is experimental and untested in-game**;
its build and offline compatibility checks passed.

`TRP_ENABLE_OPTIONAL_FEATURES` selects the release variant. `OFF` builds the standard
renderer without NR runtime integration, its shaders/UI, the Ada patch
implementation or its static library. `ON` (the default) builds the
full-feature renderer with NR and the Ada unlock. Both variants preserve native
NVIDIA capabilities and the same plugin identity. The standard build keeps NR
preferences in settings files for later use by the full build, but cannot load NR.
Build each configuration separately. The full-feature package requires
user-supplied NVIDIA files, the included settings and the runtime sharpening shader.

Detours is compiled from Nukem9's source and bundled decoder. Use the validated
revision below, placed under ignored `.dependencies/` or another local directory:

```powershell
git clone https://github.com/Nukem9/detours.git .dependencies/detours
git -C .dependencies/detours checkout cc5a2e4a58ef462821877b35ad30215f0e16bba1
```

Set `ARP_DETOURS_DIR` if that checkout is elsewhere. Detours, Zydis and Zycore
notices are retained in [third-party notices](../THIRD-PARTY.md).

From the repository directory, using your actual dependency locations:

```powershell
cmake --preset arp-nvidia `
  "-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  "-DTRP_COMMONLIBSSE_NG_DIR=C:/deps/CommonLibSSE-NG" `
  "-DTRP_NGX_SDK_DIR=C:/deps/Streamline/external/ngx-sdk" `
  "-DTRP_STREAMLINE_INCLUDE_DIR=C:/deps/Streamline/include"
cmake --build --preset arp-nvidia --parallel 6
```

An existing vcpkg installation can be reused with
`-DVCPKG_INSTALLED_DIR=<installed directory>` and `-DVCPKG_MANIFEST_INSTALL=OFF`.
If the NGX import library is elsewhere, set `TRP_NGX_LIB` to its
`nvsdk_ngx_d.lib` file. Local paths can be placed in ignored
`CMakeUserPresets.json`; dependencies can be kept under ignored `.dependencies/`.

`ARPBaseline` builds `TheosRenderPipeline.dll`, including its ImGui integration,
under `out/build/nvidia/Release`. It performs no packaging, deployment or game
launch.

`ARPDetours` builds the dependency from source, including its bundled decoder.
No precompiled Detours libraries are stored in this archive.

## Runtime files

A complete installation also needs the configuration/shaders under `package/`,
matching SKSE64/Address Library and the separately supplied NVIDIA runtimes:

- `SKSE/Plugins/TheosRenderPipeline/nvngx_dlss.dll`
- For NR in the full-feature build: `SKSE/Plugins/TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll`
- Under `SKSE/Plugins/TheosRenderPipeline/NVIDIA/Streamline/`: `nvngx_dlssg.dll`, `sl.common.dll`,
  `sl.dlss_g.dll`, `sl.interposer.dll`, `sl.pcl.dll` and `sl.reflex.dll`.

Keep the matching vendor license files with the runtimes. Existing runtime
compatibility checks are retained. The standard build omits NR controls; the full build retains
all NR/MFG controls. Build output alone
is not a complete installation; use a separately assembled release package and
the [installation guide](../package/README.md).

## Release metadata

Display version and portable diagnostic paths are configured in
`cmake/ReleaseMetadata.cmake`. The MSVC path mapping uses
`/experimental:deterministic`; the validated toolchain is Visual Studio 2022
MSVC 19.44.35215. CommonLib's diagnostic paths are also mapped when its source
is supplied outside the checkout. Keep other dependencies under `.dependencies/`
when reproducing the release's relative diagnostic paths. Plugin filenames and SKSE version
identity remain separate from the release display version.
