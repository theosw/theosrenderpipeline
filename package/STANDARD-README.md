# Theo's Render Pipeline — Standard, 0.2.3

DLSS/DLAA, native NVIDIA frame generation, Neural Rendering, NVIDIA Reflex and
native-resolution menus for Skyrim. All eight NVIDIA runtime DLLs are included.

Version 0.2.3 adds the Nolvus keyboard-input fallback, optional NR peripheral
compression and optional combined preparation. Both NR optimizations default
to off; enable them separately in the Neural Rendering controls and use Apply now.
It also corrects menu percentage text and prevents a renderer shutdown when
NVIDIA returns a VRAM-budget warning after accepting frame-generation settings.
This correction does not reduce memory pressure itself.

Version 0.2.2 enables NR with native DLAA, before or after anti-aliasing, and
adds startup identification and configuration diagnostics. The 0.2.1 startup
fix for saved 78% Ultra Quality DLSS is retained.

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
hardware/runtime support. RTX 30 requires Universal installed after Standard,
even with frame generation off; Standard alone cannot initialize its host there.
AMD/Intel are not supported. ENB is optional. Community Shaders and optional
ReShade setup are described below.

## Settings

Defaults are DLSS at 67%, preset K, sharpening enabled, frame generation x2 and
NR off. Enable Neural Rendering in the End menu; no separate NR download is
needed. Before DLSS/one pass is the default placement. After DLSS, two passes,
input scaling and tuning remain available. NR and frame generation are independent.
Both NR placements now work with native DLAA. At 100% NR input scale, both
process the native resolution, so placement alone does not reduce inference cost.

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
HDR is not supported in this release. Keep CS HDR off; it can leave the TRP
menu invisible and frame generation inactive.

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

For the RTX 40 MFG unlock or experimental RTX 30 support, install the matching 0.2.3 Universal
ZIP after Standard in MO2's left pane. Let Universal win file conflicts. Universal uses
the NVIDIA runtimes included here, including NR.

Both editions contain settings files; the later mod's files win. Switching
editions may change settings. Disable Universal to return to Standard. Both editions
support NR; Standard excludes Ada/Ampere compatibility code.

## Nolvus Awakening (candidate)

Nolvus uses the same editions; RTX 30 and RTX 40 multipliers above x2 require
Universal after Standard. The PR #33 Input Test ZIP, if supplied separately,
goes below both and inherits their settings/runtimes. It is not included in
the original 0.2.2 release ZIPs. Keep the base mods enabled, and disable the
test overlay after closing Skyrim to restore the previous renderer.

Disable competing ENB Anti-Aliasing and ENB Frame Generation components,
including their dedicated settings overrides where installed. Keep the base
SSE Display Tweaks, ENB and ReShade preset; disable SSE ReShade Helper.
End can conflict with STB Active Effects. With Skyrim closed, set
`ToggleOverlay=0x79` under `[Hotkeys]` in the winning TRP INI for F10, if free.
Click outside an active text field before closing with F10.

The candidate has positive F10 input, x5, both NR placements and Wheeler
feedback on Nolvus Awakening 6.0.20 / Skyrim 1.5.97 with Universal, RTX 4080
SUPER, ENB 0.504 and ReShade 6.3.1. Standard gameplay remains untested. Early
OAR/IED loading panels have a known resolution limitation; the test log retains
two Streamline RSYNC errors, and physical cadence remains unverified.
See [PR #33](https://github.com/theosw/theosrenderpipeline/pull/33) for candidate status.

## Compatibility and reports

Standard 0.1.4 has positive reports with Cabbage ENB and the CS setup above,
with native x2 and both NR placements and no recorded NR/host failure counters.
The ENB run retains two recurring Streamline RSYNC errors. Both shading setups
use the same renderer DLL; enable only the intended shading setup in each profile.
Skyrim 1.5.97, 1.6.640 and 1.7.104 remain experimental. The Universal Nolvus
candidate above has limited 1.5.97 gameplay evidence; Standard on 1.5.97,
and either edition on 1.6.640 or 1.7.104, remain untested in-game.
Universal 0.2.2 has positive ENB/RTX 4080 SUPER DLAA/NR feedback and an RTX 3060
Laptop/CS volunteer report confirming FG and NR execution. RTX 30 remains
experimental, with grass-edge artifacting and occasional hitches reported.
Standard's DLAA/NR correction passes offline checks; the local gameplay test
used Universal. Native RTX 50-series operation and physical cadence remain unverified.

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
