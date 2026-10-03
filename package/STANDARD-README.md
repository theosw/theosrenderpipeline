# Theo's Render Pipeline — Standard, 0.3.7

Version 0.3.7 adds menu-key rebinding under Advanced > Menu key and live
experimental HDR output switching with Apply. The first HDR activation briefly
pauses frame generation; later switches are immediate. Save as default remembers
the menu key and HDR setting. Both editions retain NR and their existing
frame-generation routes, with unchanged NVIDIA runtime DLLs and feature defaults.

Version 0.3.6 adds automatic menu scaling and title-bar Zoom controls, with
an embedded font; no separate font installation is required. Save as default
remembers zoom, window geometry and the divider. GPU failure diagnostics retain
the first fault and support optional DRED evidence (off by default). Both editions
retain NR and existing compatibility routes; NVIDIA runtime DLLs are unchanged.

Version 0.3.5 enables DLSS-G UI recomposition by default to reduce HUD
ghosting; toggle it live under Frame generation. GPU retirement waits now
continue while the fence progresses, avoiding permanent black screens from
recoverable multi-second stalls, and log extended waits. A fence with no
progress still fails after 20 seconds. ReShade regression tests are expanded.
Both editions retain NR, existing compatibility routes and unchanged NVIDIA
runtimes. HDR remains experimental and defaults off.

RTX 20 users need Universal's compatibility path installed after Standard.
This edition supplies the retained runtime bundle and all 0.2.5 fixes.

DLSS/DLAA, native NVIDIA frame generation, Neural Rendering, NVIDIA Reflex and
native-resolution menus for Skyrim. All eight NVIDIA runtime DLLs are included.

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

Apply changes this session. Save as default also saves settings and window
layout. Discard drops unapplied edits. DLSS/DLAA mode and render scale need a restart.

To change the menu key, open **Advanced**, click the **Menu key** button and
press a new key, then Apply or Save as default. Escape cancels; **Reset to End**
restores the default. Pick a key your other mods and Skyrim controls don't use.
End, function keys, Insert, Page Up/Down, Pause and Scroll Lock also close the
menu while you type in a field; with other keys, click outside the field first.

If the NR DLL is removed, its controls become unavailable until it is restored
and Skyrim restarted; DLSS/frame generation remain available.

Live FPS and related measurements stay beside the controls. Resize the window
and drag the column divider to change the layout. The pipeline bar above the
tabs shows the applied rendering order. Advanced contains Lab mode for detailed
runtime information and the information needed for problem reports.

The Neural Rendering tab contains **Base** and named presets. Presets change
only their edited settings; other settings follow Base. Sharpening is here too.
Pass 2 follows Pass 1 until you edit a Pass 2 setting; **Match Pass 1** relinks it.
Both passes use the same placement, and two passes can substantially increase
inference cost. See [preset files and weather/time controls](APPEARANCE-PROFILES.md).

## Community Shaders

Keep CS upscaling enabled. Disable CS frame generation and CS Reflex: TRP supplies
both. CS controls upscaling, render scale, sharpening and colour; TRP controls FG
and NR. Assign CS a separate menu key, such as F8, to avoid End conflicts with
TRP or KreatE, or change TRP's key under Advanced > Menu key. Do not use F8 if
it is already assigned to ReShade or a capture tool.

Standard 0.1.4 was tested with Bottle's CS build/Effects 11 on Skyrim 1.6.1170
and an RTX 4080 SUPER, with native x2 and both NR placements. Other CS builds
need confirmation.
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
for its scope. Standard HDR gameplay is still unverified. Recurring NVIDIA
flip-queue errors remain unresolved; physical frame cadence and other GPUs are
unverified.

Bottled's new experimental NR implementation is not yet validated alongside
TRP NR. Its runtime can remain loaded even with its NR checkbox off, preventing
TRP NR from starting. Do not switch between the two NR implementations in the
same launch; a restart and isolated runtime ownership are required. This release
does not resolve that conflict.

## ReShade (optional)

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

The shared ReShade integration passed offline checks in both editions.
ReShade 6.8 was tested with Universal on Skyrim 1.6.1170, Cabbage ENB and
an RTX 4080 SUPER: ordinary 6.8.0.2158 and full add-on 6.8.0.2155, with
early NR and x4. Sky Reflection Fix's ReShade registration was excluded;
Rumble passed a separate initial test. ReGrade+, combined third-party add-ons,
Standard gameplay and CS with ReShade remain unverified. These were earlier
development builds; the exact 0.3.7 package still needs its final game test.

## With the Universal edition

For the RTX 40 MFG unlock or experimental RTX 20/30 support, install the matching
0.3.7 Universal ZIP after Standard in MO2's left pane. Let Universal win file conflicts. Universal uses
the NVIDIA runtimes included here, including NR.

Both editions contain settings files; the later mod's files win. Switching
editions may change settings. On cards with native FG support, disable Universal
to return to Standard. RTX 20/30 still require Universal even with interpolation
off. Both editions support NR; Standard excludes Ada/Ampere/Turing compatibility code.

## Nolvus Awakening

Nolvus uses the same editions; RTX 30 and RTX 40 multipliers above x2 require
Universal after Standard. The keyboard fallback from PR #33 is included from
0.2.3 onward; a separate Input Test ZIP is not needed for this release.

Disable competing ENB Anti-Aliasing and ENB Frame Generation components,
including their dedicated settings overrides where installed. Keep the base
SSE Display Tweaks, ENB and ReShade preset; disable SSE ReShade Helper.
End can conflict with STB Active Effects. Change the menu key under
**Advanced > Menu key**, for example to F10 if free, then Save as default.

The candidate has positive F10 input, x5, both NR placements and Wheeler
feedback on Nolvus Awakening 6.0.20 / Skyrim 1.5.97 with Universal, RTX 4080
SUPER, ENB 0.504 and ReShade 6.3.1. Standard gameplay remains untested. Early
OAR/IED loading panels have a known resolution limitation; the test log retains
two Streamline RSYNC errors, and physical cadence remains unverified.
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
