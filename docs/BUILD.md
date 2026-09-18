# Building the plugin

Requires Windows, Visual Studio 2022 C++ tools/Windows SDK, CMake and vcpkg.
The build target and SKSE plugin identity are `TheosRenderPipeline`.

Supply CommonLibSSE-NG source, Streamline public headers and the NVIDIA NGX SDK
headers/import library. This branch uses CommonLibSSE-NG 8.1.0
and Streamline 2.11.1 headers. CMake reads the other libraries from `vcpkg.json`.
Runtime DLLs are not required to compile the plugin.

Use the exact CommonLib revision below for format-5 Address Library loading,
1.7 runtime classification and versioned engine layouts. The CMake build uses
source, with SE/AE enabled and VR disabled. Its patch-boundary diagnostics
remain enabled and fetch the HDE64 decoder from MinHook v1.3.4. The dependency's
GPL/modding-exception terms and decoder notices are in [THIRD-PARTY.md](../THIRD-PARTY.md).

```powershell
git clone --branch v8.1.0 https://github.com/alandtse/CommonLibSSE-NG.git .dependencies/CommonLibSSE-NG
git -C .dependencies/CommonLibSSE-NG checkout 3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd
```

`arp-nvidia` builds one DLL for Steam Skyrim 1.5.97, 1.6.640, 1.6.1170 and 1.7.104, with both CommonLib
SE and AE support enabled and VR disabled. The loader accepts only those four
versions. Runtime-aware addresses and layout accessors select the appropriate
engine integration. Use matching SKSE64 and Address Library files when installing.
Both Full and Standard use this universal configuration. The component candidates have scoped Full/Standard
game evidence on 1.6.1170; the combined 0.1.3 build has not had a separate
game run. **1.5.97, 1.6.640 and 1.7.104 are experimental and
untested in-game**; the [1.7.104 port status](SKYRIM_1_7_104.md) records current evidence.

To run the optional checks in a configured build directory:

```powershell
cmake -S . -B <build-directory> -DTRP_BUILD_COMPATIBILITY_TESTS=ON
cmake --build <build-directory> --config Release --target TRPRuntimeProfileTests TRPRuntimeLayoutTests TRPSourceNvidiaFrameEvaluatorTests TRPD3D11FrameCopyTests
ctest --test-dir <build-directory> -C Release --output-on-failure
```

The `arp-nvidia` build preset builds only the renderer, so build the test targets
explicitly before running CTest.
These offline checks cover exact version admission, artwork caller isolation,
graphics/control layouts and the linked format-1/2/5 Address Library loader.
The frame evaluator check uses D3D11 WARP to verify native reconstruction ordering,
failure/reset handling and preparation of an already-completed frame without a
second upscale. The frame-copy check reads back active-area color and depth,
including cropped allocations, return-copy borders, producer overwrites, format
conversion and compute-state restoration. These tests do not execute game hooks,
load NVIDIA runtimes or validate cross-device GPU retirement.

`TRP_ENABLE_NEURAL_RENDERING` defaults to `ON` in both editions and includes NR
integration, shaders and controls. `OFF` is an explicit build without NR.
`TRP_ENABLE_OPTIONAL_FEATURES` selects MFG compatibility: `OFF` builds Standard
with native NVIDIA capabilities; `ON` (the default) adds Ada/Ampere compatibility
and its static library for Full. Build each configuration separately. Both
editions preserve the same plugin identity and complete NR controls.
The Standard download includes the NVIDIA runtimes, including NR; Full may use
Standard's runtimes when installed after it in MO2, or separately supplied files.

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
- For NR in either edition: `SKSE/Plugins/TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll`
- Under `SKSE/Plugins/TheosRenderPipeline/NVIDIA/Streamline/`: `nvngx_dlssg.dll`, `sl.common.dll`,
  `sl.dlss_g.dll`, `sl.interposer.dll`, `sl.pcl.dll` and `sl.reflex.dll`.

Keep the matching vendor license files with the runtimes. Existing runtime
compatibility checks are retained. Both editions retain NR controls; Full also
retains Ada/Ampere MFG compatibility. Build output alone
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
