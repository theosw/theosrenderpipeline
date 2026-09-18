# Theo's Render Pipeline — Standard, 0.1.4 candidate

DLSS/DLAA, native NVIDIA frame generation, Neural Rendering, NVIDIA Reflex and
native-resolution menus for Skyrim. All eight NVIDIA runtime DLLs are included.

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
AMD/Intel and ReShade integration are not supported. ENB is optional. Community Shaders setup is described below.

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
TRP or KreatE. Do not use F8 if it is already assigned to a capture tool.

Earlier Full checkpoints were tested with Bottle's CS build/Effects 11 on
Skyrim 1.6.1170 and an RTX 4080 SUPER. Other CS builds need confirmation.

## With the Full edition

For the RTX 40 MFG unlock or experimental RTX 30 support, install the matching 0.1.4 Full
ZIP after Standard in MO2's left pane. Let Full win file conflicts. Full uses
the NVIDIA runtimes included here, including NR.

Both editions contain settings files; the later mod's files win. Switching
editions may change settings. Disable Full to return to Standard. Both editions
support NR; Standard excludes Ada/Ampere compatibility code.

## Compatibility and reports

The combined 0.1.4 Standard candidate has not yet been tested in-game. Earlier
Full CS/NR and Standard menu checks apply only to their recorded builds.
Skyrim 1.5.97, 1.6.640 and 1.7.104 are experimental and untested in-game.
Actual RTX 30 execution in Full, native RTX 50-series operation, HDR appearance
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
