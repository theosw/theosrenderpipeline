# Theo's Render Pipeline — Universal, 0.3.5

Version 0.3.5 enables DLSS-G UI recomposition by default to reduce HUD
ghosting; toggle it live under Frame generation. GPU retirement waits now
continue while the fence progresses, avoiding permanent black screens from
recoverable multi-second stalls, and log extended waits. A fence with no
progress still fails after 20 seconds. ReShade regression tests are expanded.
Both editions retain NR, existing compatibility routes and unchanged NVIDIA
runtimes. HDR remains experimental and defaults off.

Version 0.3.0 added experimental RTX 20 DLSS-G/MFG compatibility and retains
all 0.2.5 fixes. The separate RTX 2060 test records x2/x3/x4/x6, NR and loading
recovery. The combined release has positive ENB/RTX 4080 SUPER regression feedback
with x4/NR and loading recovery. Broader RTX 20 coverage remains unverified.
See RTX20-TEST.md.

Version 0.2.5 improves startup compatibility with existing renderer hooks,
including SSE Display Tweaks BorderlessUpscale and Community Shaders. Rejected
Save/Apply actions now show their reason, hidden dependent settings no longer
block unrelated edits, and startup diagnostics explain observed NVIDIA App
frame-generation override conflicts. NVIDIA runtimes and defaults are unchanged.

Version 0.2.4 adds a reorganized menu with live measurements beside settings,
a saved resizable layout and independent NR pass controls. NR keyboard shortcuts
are now opt-in to avoid shared-key conflicts. Existing runtime DLLs, the Nolvus
input correction and the VRAM-budget warning fix are retained. Peripheral
compression and combined preparation remain optional and default to off.

Version 0.2.2 enables NR with DLAA, before or after anti-aliasing, and improves
experimental RTX 30 compatibility. Older INIs now use the packaged compatibility
default when the key is missing; explicit opt-outs remain respected. The 0.2.1
Ultra Quality fix is retained. Universal includes the RTX 40 MFG unlock.

DLSS/DLAA, frame generation, Neural Rendering and native-resolution UI for Skyrim.
This package includes the full renderer, configuration and sharpening shader.
It requires no other Theo's Render Pipeline package. NVIDIA DLLs are supplied
separately: download the SR/FG files below, and the NR runtime if you want NR.
Alternatively, install Standard first and Universal after it in MO2; Standard
0.3.5 supplies the runtime bundle (0.2.5 retains the same DLLs). The NR-enabled
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

**End** opens settings. **Apply** changes this session. **Save as default**
also saves settings and window layout; **Discard** drops unapplied edits. DLSS/DLAA mode
and render scale changes take effect after restarting Skyrim.

Live FPS and related measurements stay beside the controls. Resize the window
and drag the column divider to change the layout. The pipeline bar above the
tabs shows the applied rendering order. Advanced contains Lab mode for detailed
runtime information and the information needed for problem reports.

The Neural Rendering tab contains **Base** and named presets. Presets change
only their edited settings; other settings follow Base. Sharpening is here too.
Pass 2 follows Pass 1 until you edit a Pass 2 setting; **Match Pass 1** relinks it.
Both passes use the same placement, and two passes can substantially increase
inference cost. See [preset files and weather/time controls](APPEARANCE-PROFILES.md).

- DLSS starts at 67%, preset K, with sharpening enabled.
- Frame generation starts on at x2. Choose x4 or another supported multiplier
  in End. RTX 30-series uses experimental Ampere compatibility, RTX 40-series
  uses the Ada unlock, and RTX 50-series uses native capabilities. The FG toggle
  leaves the rendering host active. The saved `SourceDLSSGMFGUnlock` setting
  controls both compatibility paths; leave it enabled for RTX 30 cards.
- NR starts off. After supplying its DLL, enable it with Before DLSS/one pass
  as the default placement. After DLSS, two passes, input scaling and tuning
  are also available. Both placements now work with native DLAA. At 100% NR
  input scale, DLAA gives both placements the same resolution; changing their
  order alone does not reduce inference resolution.
- Reflex and GPU measurements are enabled.
- **Advanced → Request loading-screen artwork** defaults on. It asks Skyrim
  to select artwork on eligible cell transitions; startup loading is unchanged.

The renderer includes external ImGui integration. Both included INIs provide
the packaged configuration; `RCAS.hlsl` is the runtime sharpening shader.

## Requirements and reports

The same renderer DLL targets Skyrim 1.6.1170 and experimental 1.5.97/1.6.640/1.7.104.
Install SKSE64 and Address Library matching your game, the x64 Microsoft
Visual C++ runtime and a compatible NVIDIA GPU/driver. DLSS-G-compatible hardware
is required even with frame generation switched off. AMD/Intel are not supported.

ENB is optional. With Community Shaders, keep CS upscaling enabled and disable
CS frame generation and CS Reflex. TRP provides FG/Reflex/NR while CS retains
upscaling, render scale, sharpening and colour. Assign CS a separate menu key,
such as F8, avoiding keys already assigned to ReShade or capture tools. TRP keeps End.
With CS, HDR is experimental and works only through CS HDR Display (tested with CS 1.9.1,
HDR Display 1.2.2 and Universal on one RTX 4080 SUPER; Standard is untested).
Enable Windows HDR and use borderless windowed. In CS's HDR Display settings, set
peak brightness to your monitor's value and start paper white near the Windows SDR
content brightness. Weathers built for ENB, such as NAT.ENB, need CS Effects 11
with a preset; without one the image is much darker in both SDR and HDR. Prefer
one NR pass or NR before upscaling; two full-resolution passes after upscaling
are expensive.
Without CS (for example with ENB), the experimental **Image > HDR output** setting
expands TRP's finished SDR image to HDR10 with its own paper white, peak and UI
brightness. It needs Windows HDR, is off by default and applies after saving and
restarting. Clipped highlights cannot be recovered. It has one ENB game test
(Universal, RTX 4080 SUPER); see the [HDR guide](https://github.com/theosw/theosrenderpipeline#hdr-experimental)
for its scope. Recurring NVIDIA flip-queue errors remain unresolved; physical
frame cadence and other GPUs are unverified.

Earlier 0.1.4 builds have positive Skyrim 1.6.1170/RTX 4080 SUPER reports with Cabbage
ENB and Bottle's Community Shaders build/Effects 11, including logged x4 and
both NR placements in each setup. Universal also logged x6 in the CS run. Recurring
Streamline RSYNC errors remain recorded in some tests; physical frame cadence
has not been validated.
Version 0.2.2 has positive ENB/RTX 4080 SUPER feedback for NR before and after
native 5120x1440 DLAA, with x4. An RTX 3060 Laptop volunteer using CS confirmed
frame generation and NR; logs record x2/x3/x4 outputs. RTX 30 support remains
experimental, with grass-edge artifacting and occasional hitches reported.
Other RTX 30 configurations, native RTX 50 operation and other CS builds need
further testing. NR can be expensive on RTX 30: start with it off, then try one
pass before upscaling and reduce NR input resolution if needed.

## ReShade (optional)

ReShade 6.8 was tested with Universal on Skyrim 1.6.1170, Cabbage ENB and
an RTX 4080 SUPER: ordinary 6.8.0.2158 and full add-on 6.8.0.2155, with
early NR and x4. Sky Reflection Fix's ReShade registration was excluded;
Rumble passed a separate initial test. ReGrade+, combined third-party add-ons,
Standard gameplay and CS with ReShade remain unverified. These were earlier
development builds; the exact 0.3.5 package still needs its final game test.

Keep your existing ReShade installation, preset and hotkeys. **Disable SSE
ReShade Helper.** TRP supplies the effects and overlay stages. Effects run after
upscaling by default; use **Advanced → ReShade** to change this.
Changing placement may reload shaders. Give ReShade, CS and TRP different menu keys.

Close the ReShade overlay before taking a corrected screenshot. ReShade's
screenshot key keeps its folder, naming and format; after ReShade saves its file,
TRP replaces it with a subsequent real frame containing DLSS, NR and the HUD.
Keep the view still until the replacement finishes: this is not an exact capture
of the moment the key was pressed. Screenshot post-save commands see the original
ReShade file before replacement.

With the ReShade overlay open, HDR output, an unsupported format or a capture
failure, TRP preserves ReShade's original file (which may contain only the HUD).
Optional before-effects and overlay copies are unchanged. ENB's screenshot key
sees the image before DLSS and NR; Steam or Windows capture can capture the
displayed image without this replacement delay.

The renderer was tested with and without ReShade 6.3.3.1921 on Skyrim 1.6.1170,
Cabbage ENB and RTX 4080 SUPER, with x4 and NR before upscaling. CS with ReShade,
other ReShade versions and other effect/NR placements still need game testing.
Keep Native UI enabled for the tested world-only effects setup.

## Nolvus Awakening

The keyboard fallback in PR #33 has scoped positive Nolvus Awakening 6.0.20 /
Skyrim 1.5.97 feedback: Universal, RTX 4080 SUPER, ENB 0.504, ReShade 6.3.1,
F10 input, x5, both NR placements and Wheeler. The fix is included from 0.2.3
onward; a separate Input Test ZIP is not needed for this release.

Disable competing ENB Anti-Aliasing and ENB Frame Generation components,
including their dedicated settings overrides where installed. Keep the base
SSE Display Tweaks, ENB and ReShade preset; disable SSE ReShade Helper.
End can conflict with STB Active Effects. With Skyrim closed, set
`ToggleOverlay=0x79` under `[Hotkeys]` in the winning TRP INI for F10, if free.
Click outside an active text field before closing with F10.
Early OAR/IED loading panels remain a known resolution limitation. The test log
retains two Streamline RSYNC errors; physical cadence and unreported menu/cell
transitions remain unverified.
See [PR #33](https://github.com/theosw/theosrenderpipeline/pull/33) for the input fix.

## NR shortcuts

The NR menu checkbox works independently of keyboard shortcuts. Bracket
shortcuts are disabled by default, including for older INIs without the new key.
To opt in, set `EnableNRHotkeys=true` under `[Hotkeys]` in the winning
`TheosRenderPipeline.ini` and restart Skyrim. On a US keyboard, `[` turns NR
off and `]` turns it on for the session, even with the TRP menu closed.
They are suppressed while editing text in TRP's menu. Other mods can share
these keys. Use Save as default to retain an NR state for future launches.
The existing `ToggleOverlay` menu binding is independent.

## Menu zoom

The menu sizes itself from the output height: 1080p is 100% and 4K is 200%.
To make it larger or smaller, use **-** and **+** next to **Zoom** at the right
of the menu's title bar; each press changes the zoom by 25%. Click the
percentage between them to return to the automatic zoom. Under Advanced, clear
**Automatic zoom** to set **Zoom** from 75% to 300%. Changes apply immediately;
Save as default remembers them with the window layout. On smaller outputs the
zoom is limited so the menu still fits. The INI equivalent is `UIScale` under
`[Overlay]`, where 0 is automatic.

## Experimental game versions

**Skyrim 1.5.97, 1.6.640 and 1.7.104 remain experimental.** The candidate Nolvus
result above covers a limited 1.5.97 configuration; 1.6.640 and 1.7.104 remain
untested in-game. Build and offline compatibility checks passed. Use matching SKSE64 and Address Library, and please
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

In 0.2.2, a missing `Experimental/SourceDLSSGMFGUnlock` key uses the packaged
`true` default. Explicit `false` remains respected. RTX 30 requires Universal
and this setting enabled even when interpolation is off. If startup fails,
include `TheosRenderPipeline.log`; its opening lines identify the edition,
source revision, renderer path and effective startup setting. Standard alone
does not supply the compatibility path required by RTX 30.
