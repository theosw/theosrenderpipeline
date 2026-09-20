# Theo's Render Pipeline — Standard, 0.2.2

DLSS/DLAA, native NVIDIA frame generation, Neural Rendering, NVIDIA Reflex and
native-resolution menus for Skyrim. All eight NVIDIA runtime DLLs are included.

The 0.2.1 startup fix for saved 78% Ultra Quality DLSS is retained, preserving
the selected render resolution and settings.

## Install

1. Install and enable this ZIP in MO2. Disable competing upscaler/frame-generation mods.
   If using Community Shaders, keep CS enabled and follow the CS setup below.
2. Enable Hardware-Accelerated GPU Scheduling in Windows graphics settings and
   restart your PC if you changed it. Use windowed or borderless mode in Skyrim.
3. Launch SKSE through MO2. Press End to open settings.

Use matching SKSE64 and Address Library for Steam Skyrim 1.5.97, 1.6.640,
1.6.1170 or 1.7.104, the x64 Microsoft Visual C++ runtime and a compatible
NVIDIA GPU/driver. Standard requires native DLSS-G hardware support even with
frame generation off. RTX 40-series uses x2; higher multipliers require native
hardware/runtime support. Standard does not add RTX 30 frame generation.
AMD/Intel are not supported. ENB is optional. Community Shaders and optional
ReShade setup are described below.

## Settings

Defaults are DLSS at 67%, preset K, sharpening enabled, frame generation x2 and
NR off. Enable Neural Rendering in the End menu; no separate NR download is
needed. Before DLSS/one pass is the default placement. After DLSS, two passes,
input scaling and tuning remain available. NR and frame generation are independent.

Apply now changes this session. Save as default also saves settings. Discard
changes drops unapplied edits. DLSS/DLAA mode and render scale need a restart.
If the NR DLL is removed, its controls become unavailable until it is restored
and Skyrim restarted; DLSS/frame generation remain available.

## Community Shaders

Keep CS upscaling enabled. Disable CS frame generation and CS Reflex: TRP supplies
both. CS controls upscaling, render scale, sharpening and colour; TRP controls FG
and NR. Assign CS a separate menu key, such as F8, to avoid End conflicts with
TRP or KreatE. Do not use F8 if it is already assigned to ReShade or a capture tool.

Standard 0.1.4 was tested with Bottle's CS build/Effects 11 on Skyrim 1.6.1170
and an RTX 4080 SUPER, with native x2 and both NR placements. Other CS builds
need confirmation.
The tested CS setup uses SDR. CS HDR remains unverified and has an unresolved
report of an invisible TRP menu and inactive frame generation.

## ReShade (optional)

Keep your existing ReShade installation, preset and hotkeys. **Disable SSE
ReShade Helper.** TRP supplies the effects and overlay stages. Effects run after
upscaling by default; use **Advanced → ReShade before upscaling** to change this.
Changing placement may reload shaders. Give ReShade, CS and TRP different menu keys.

The shared ReShade integration passed offline checks in both editions. Gameplay
was tested in Universal with ReShade 6.3.3.1921, Skyrim 1.6.1170, Cabbage ENB and
RTX 4080 SUPER. Standard ReShade gameplay, CS with ReShade, other ReShade versions
and other effect/NR placements still need testing. Keep Native UI enabled for
the tested world-only effects setup.

## With the Universal edition

For the RTX 40 MFG unlock or experimental RTX 30 support, install the matching 0.2.2 Universal
ZIP after Standard in MO2's left pane. Let Universal win file conflicts. Universal uses
the NVIDIA runtimes included here, including NR.

Both editions contain settings files; the later mod's files win. Switching
editions may change settings. Disable Universal to return to Standard. Both editions
support NR; Standard excludes Ada/Ampere compatibility code.

## Compatibility and reports

Standard 0.1.4 has positive reports with Cabbage ENB and the CS setup above,
with native x2 and both NR placements and no recorded NR/host failure counters.
The ENB run retains two recurring Streamline RSYNC errors. Both shading setups
use the same renderer DLL; enable only the intended shading setup in each profile.
Skyrim 1.5.97, 1.6.640 and 1.7.104 are experimental and untested in-game.
Actual RTX 30 execution in Universal, native RTX 50-series operation, HDR appearance
and physical frame cadence remain unverified.

For reports, include TRP-STANDARD-PACKAGE.txt, GPU/driver, game/mod versions,
settings and TheosRenderPipeline.log/skse64.log. Logs are normally under
Documents/My Games/Skyrim Special Edition/SKSE/.

## Included runtimes and notices

Seven unchanged production DLLs from Streamline 2.14.1 provide DLSS/DLSS-G
310.9.1 and the required Streamline components. The included NR runtime is
310.8, matching the previously tested DLSS5 reshade/Build16 binary.
See LICENSE and THIRD-PARTY.md for renderer terms, source credits and NVIDIA
RTX SDK/DLSS terms. NVIDIA-LICENSES.txt retains the accompanying runtime notices.
NVIDIA components retain their own terms and are not relicensed under the
renderer's GPL license. This software contains source code provided by NVIDIA Corporation.

Version 0.2.2 adds startup identification and compatibility-setting diagnostics.
Standard does not include RTX 30 compatibility; install matching Universal
after Standard so Universal wins the renderer DLL conflict. This is required
even with frame generation switched off. RTX 30 execution remains unverified.
