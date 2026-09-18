# Theo's Render Pipeline — Full, 0.1.4 candidate

Adds Community Shaders integration and NR Ratio/runtime checks. Standard now also
includes NR; Full adds the RTX 40 MFG unlock and experimental RTX 30 compatibility.
The combined 0.1.4 builds still need their own game acceptance.

DLSS/DLAA, frame generation, Neural Rendering and native-resolution UI for Skyrim.
This package includes the full renderer, configuration and sharpening shader.
It requires no other Theo's Render Pipeline package. NVIDIA DLLs are supplied
separately: download the SR/FG files below, and the NR runtime if you want NR.
Alternatively, install matching 0.1.4 Standard first and Full after it in MO2; the NR-enabled
Standard download supplies all eight runtimes, including NR. In that setup,
skip the runtime downloads below.

## Install

1. Install and enable this ZIP in **MO2**. Disable other upscaler and
   frame-generation mods. Keep Community Shaders enabled if you use it; follow
   the CS setup below.
2. Download the **SDK ZIP** under **Assets** on the
   [NVIDIA Streamline 2.14.1 page](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1).
   Extract it and open its `bin/x64` folder.
3. In MO2, right-click **Theo's Render Pipeline → Open in Explorer**.
   Copy the seven DLLs into the existing folders shown below.

| Files from `bin/x64` | Folder inside this mod |
| --- | --- |
| `nvngx_dlss.dll` | `SKSE/Plugins/TheosRenderPipeline/` |
| `nvngx_dlssg.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.interposer.dll`, `sl.pcl.dll`, `sl.reflex.dll` | `SKSE/Plugins/TheosRenderPipeline/NVIDIA/Streamline/` |

4. Enable **Hardware-Accelerated GPU Scheduling** in Windows graphics settings.
   Restart your PC if you changed it. Use **windowed or borderless mode** in Skyrim.
5. Launch **SKSE through MO2**. Press **End** to open settings.

The destination folders are included in the ZIP. Use the `bin/x64` files
from the download, not its debug or development folders.

## Optional Neural Rendering (NR)

On the [DynamicShaderFrameGen files page](https://www.nexusmods.com/skyrimspecialedition/mods/190154?tab=files),
choose **Old files → DLSS5 reshade**, version 1, uploaded **31 August 2026**
(148.2 MB). Use **Manual download**, extract it, and copy only this DLL:

| File | Folder inside this mod |
| --- | --- |
| `nvngx_dlssnr.dll` | `SKSE/Plugins/TheosRenderPipeline/NVIDIA/` |

Then enable **Neural Rendering** in the End menu. Leave it off if you skip this
download. You do not need ReShade or the other files from that archive.

NR controls are unavailable until its optional DLL is installed. If saved
settings enable NR but the DLL is missing, Skyrim starts with NR off and keeps
DLSS/frame generation available. Install the DLL and restart to use NR.

These downloads are the tested set: SR/FG 310.9.1, Streamline 2.14.1 and NR 310.8.
Newer runtimes use the same filenames and folders; compatibility may vary.

## Controls

**End** opens settings. **Apply now** changes this session. **Save as default**
also saves settings; **Discard changes** drops unapplied edits. DLSS/DLAA mode
and render scale changes take effect after restarting Skyrim.

- DLSS starts at 67%, preset K, with sharpening enabled.
- Frame generation starts on at x2. Choose x4 or another supported multiplier
  in End. RTX 30-series uses experimental Ampere compatibility, RTX 40-series
  uses the Ada unlock, and RTX 50-series uses native capabilities. The FG toggle
  leaves the rendering host active. The saved `SourceDLSSGMFGUnlock` setting
  controls both compatibility paths; leave it enabled for the RTX 30 test.
- NR starts off. After supplying its DLL, enable it with Before DLSS/one pass
  as the default placement. After DLSS, two passes, input scaling and tuning
  are also available.
- Reflex and GPU measurements are enabled.
- **Advanced → Request loading-screen artwork** defaults on. It asks Skyrim
  to select artwork on eligible cell transitions; startup loading is unchanged.

The renderer includes external ImGui integration. Both included INIs provide
the packaged configuration; `RCAS.hlsl` is the runtime sharpening shader.

## Requirements and reports

The same renderer DLL targets Skyrim 1.6.1170 and experimental 1.5.97/1.6.640/1.7.104.
Install SKSE64 and Address Library matching your game, the x64 Microsoft
Visual C++ runtime and a compatible NVIDIA GPU/driver. DLSS-G-compatible hardware
is required even with frame generation switched off. AMD/Intel and ReShade
integration are not supported.

ENB is optional. With Community Shaders, keep CS upscaling enabled and disable
CS frame generation and CS Reflex. TRP provides FG/Reflex/NR while CS retains
upscaling, render scale, sharpening and colour. Assign CS a separate menu key,
such as F8, avoiding keys already assigned to capture tools. TRP keeps End.

Earlier candidates received scoped Skyrim 1.6.1170/RTX 4080 SUPER checks with
ENB and Bottle's Community Shaders build/Effects 11. Both CS NR placements and
Ratio controls were accepted. The combined 0.1.4 build still needs its own run.
Other CS builds, RTX 30 execution, native RTX 50 operation and HDR appearance
remain unverified.

**Skyrim 1.5.97, 1.6.640 and 1.7.104 are experimental and untested in-game.** Build and offline
compatibility checks passed. Use matching SKSE64 and Address Library, and please
report your results with the game/mod versions and renderer log.
Third-party ImGui integration covers the listed producer builds; older or
SE-specific versions of those mods need their own compatibility checks.

Include `TRP-FULL-PACKAGE.txt`, your GPU/driver, game/mod versions, settings and
`TheosRenderPipeline.log`/`skse64.log` when reporting a problem. The package
identity links to the matching source revision in the
[source repository](https://github.com/theosw/theosrenderpipeline).
Logs are normally under `Documents/My Games/Skyrim Special Edition/SKSE/`.

See `LICENSE` and `THIRD-PARTY.md` for project terms and attribution.
Notices for SDK code included in the renderer are consolidated in
`THIRD-PARTY.md`. Separately downloaded NVIDIA files retain their accompanying terms.
