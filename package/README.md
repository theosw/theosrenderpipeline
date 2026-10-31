# 0.6.0

Universal adds GTX 10 compatibility kernels (experimental) for Neural Rendering on
sm_61 GPUs. NR stays off by default. Install Universal and the matching NR runtimes,
use NVIDIA driver 582.78 or a newer compatible Pascal (R580) driver, then select
Neural Rendering > Enable > Apply with Runtime set to Automatic. Start with one
pass before upscaling. TRP checks the rendering adapter, driver, runtime, compatibility
pack and available video-memory budget before starting NR; an admission refusal
leaves normal rendering running and explains the reason in the menu and log.
There is no fixed VRAM minimum. DLSS, DLAA and DLSS-G require NVIDIA RTX hardware;
use XeSS and optionally XeFG on Pascal. Standard keeps the GTX 10 NR controls visible
with a reason pointing to Universal. Other NR settings and saved preferences remain
unchanged. Game acceptance and broader Pascal hardware testing are still pending.

# Theo's Render Pipeline — Universal, 0.5.7

Version 0.5.7 adds an experimental option, `ReShadeGameDevice`, that runs
ReShade on the game's own device, so add-ons work the way they do without TRP
(see **ReShade on the game's device**). It also fixes a freeze when XeFG's
multiplier changed from x2 to x3-x6, or its tensor model or depth changed,
during play. See CHANGELOG.md.

Version 0.5.6 resets DLSS, NR and frame generation history when the world
map opens or closes and on the first frame of the world after a load, and
resets DLSS on a camera cut even with NR off. Motion vectors and depth now come
from the game's own targets, so another mod's texture of the same shape can no
longer become their input. It also fixes NR switching on and off in near-black
scenes, accepts key names for ToggleOverlay and gives a second Skyrim running
at the same time its own log. See CHANGELOG.md.

Version 0.5.5 lets the mouse wheel scroll the menu, changes one NR pass's
network without restarting the others, and starts the game with normal
presentation instead of failing when NVIDIA's Streamline cannot start. VSync
Auto now follows DLSS-G's own VSync requirement. It fixes Every third frame
(Lab) smearing moving detail, Save as default turning NR off while its runtime
was missing, a crash with mods that check textures through the game's device,
and XeFG refusing to start in very large load orders. See CHANGELOG.md.

Version 0.5.4 fixes saves waiting until a menu opened when Skyrim's depth of
field is off, saves Photo Mode photos with TRP's finished frame at the output
resolution, lets about 70 more menu texts be translated and fixes the menu
cursor disappearing at 75% zoom. Lab mode adds an Every third frame switch to
NR's Model rate. See CHANGELOG.md.

Version 0.5.3 adds menu languages: the menu's text now comes from a language
file, and Simplified Chinese is included (pick it with the button next to Zoom).
PrintScreen screenshots now save TRP's finished frame at the output resolution.
The Steady frame limit no longer drifts, and NR Model rate no longer recommends
it. See CHANGELOG.md.

Version 0.5.2 adds up to five NR passes, an NR Model rate that runs the model on
every other frame to reduce inference work (best with 2x frame generation),
and Frame generation > Frame limit with a Steady mode that evens out frame times.
It also fixes the vanilla main menu disappearing, NR stopping while the interface
is hidden, and a possible XeFG freeze when gameplay starts. See CHANGELOG.md.

Version 0.5.1 adds NR comparison and tuning. Neural Rendering > Compare shows
NR on and off side by side (Split) and can hold one frame (Freeze frame) while
you change settings. Ratio reconstruction gains Exposure (Fixed or Adaptive),
Broad and Detail, a steady grid before upscaling, a guard that keeps grey
shading grey, and black NR shadows without bright specks. NR settings that
rebuild the model now pause for about half a second instead of several. On
NVIDIA, the XeFG tensor model now switches with Apply, without a restart. See
CHANGELOG.md.

Version 0.5.0 redesigns the settings menu. Neural Rendering passes are folder
tabs with object rules inside each pass, short choices are segmented controls,
and presets are now called weather overrides (their files and settings are
unchanged). Object rules give each group its own strength, tone and structure in
each pass; object settings from 0.4.5 are not carried over. Frame generation adds
VSync Auto/Off/On, XeFG scene change sensitivity and, on NVIDIA, an optional XeFG
tensor model with flip metering. See CHANGELOG.md.

Version 0.4.5 fixes NR before upscaling when the interface is hidden, checks
RTX 20/30 driver prerequisites, stops repeated failed FG creation attempts,
and lets Automatic NR use an available compatible runtime when legacy 310.8
is missing. Both editions keep their runtime bundle, settings defaults and NR
controls. It also reduces repeated NR recording setup and avoids an
intermediate copy in applicable two-pass paths. Model inference is unchanged.
New gameplay acceptance is pending; see CHANGELOG.md.

Version 0.4.4 adds Automatic NR runtime selection and NVIDIA NR 310.9.16 on
RTX 20/30/40 through Universal's in-memory kernel pack. Install matching
Standard first, then Universal after it in MO2. Standard retains legacy NR
310.8 and the original NVIDIA 310.9.16 DLL. Universal adds the compatibility
pack and retains legacy INT8 for RTX 20/30. NR remains available in both
editions and defaults off. See CHANGELOG.md and the NR runtime notes below.

Version 0.4.3 adds a frame generation preset dropdown (NVIDIA Presets A, B and
C), bundles DLSS Frame Generation 310.9.16 for Preset C, adds an NR runtime
selector, and fixes flickering in dialogue with NR on. See CHANGELOG.md.

Version 0.4.2 fixes an offset copy and ghosting in generated frames while the
camera is underwater, most visible in first person near lights. It applies to
the vanilla and ENB routes; Community Shaders is unchanged.

Version 0.4.1 saves user settings separately so updates preserve them, corrects
UI alpha coverage, and adds startup troubleshooting hints. Experimental XeFG
x3-x6 is now included in Universal only.

Version 0.4.0 adds experimental Intel XeSS upscaling and XeFG frame generation
(official x2, plus experimental x3-x6), selected automatically on AMD and other
non-NVIDIA GPUs. Standard includes Intel's runtimes. Both editions include a
Community Shaders override that keeps CS frame generation and Reflex off. NVIDIA
runtimes and defaults are unchanged; NR still requires NVIDIA RTX hardware.

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
0.5.7 supplies the complete NVIDIA and Intel runtime bundles, including the
original NR 310.9.16 and the retained older NR runtime. Universal adds the
compatibility pack and legacy INT8 alternative. In that setup,
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

RTX 20/30 frame-generation compatibility requires NVIDIA driver 570.65 or
newer; 572.16 or newer is recommended. An older detected driver stops startup
with an update message. If vendor FG feature creation fails, TRP retains the
first error and requires a restart instead of retrying every frame. Include
`TheosRenderPipeline.log` when reporting it.

## NR runtime notes

Use **Neural Rendering > Runtime > Automatic** for the default selection.
Standard supplies two NR DLLs: legacy `nvngx_dlssnr.dll` (310.8) and original
NVIDIA `nvngx_dlssnr_310.9.16.dll`. Universal supplies the adjacent
`nvngx_dlssnr_310.9.16.trpk`, the experimental GTX 10 asset
`nvngx_dlssnr_310.9.16_pascal_v1.trpk`, and legacy `nvngx_dlssnr_int8.dll`.

On RTX 20/30/40, Universal first tries the original 310.9.16 DLL with the
pack. RTX 20/30 use the community FP16 compatibility kernels; RTX 40 uses
TRP's FP8 kernels. RTX 50 uses NVIDIA's original kernels without a patch.
The installer verifies the complete runtime and pack hashes, original memory
bytes and all changes before use. The original 310.9.16 file is unchanged on
disk. A safe refusal falls back to legacy INT8 then 310.8 on RTX 20/30, or
310.8 on RTX 40. The known incompatible INT8 path is blocked on RTX 40.
Standard never installs compatibility kernels and keeps 310.8 on older GPUs.

The separate community and standalone RTX 40 310.9.16 DLLs are no longer
included. Explicit custom runtime choices remain available, including older
files supplied separately. If an explicit choice names a file you removed,
choose Automatic. Older default-runtime settings migrate to Automatic.
Install the matching Standard and Universal versions when updating. A merged
update may retain older separately supplied DLLs; these remain selectable.
Automatic uses the bundled GPU policy, while an explicit old choice stays
selected. Select Automatic to move that choice to the current default. Do not
remove legacy 310.8 or INT8 merely to update; both remain supplied for compatibility.

Keep the pack in `SKSE/Plugins/TheosRenderPipeline/NVIDIA/` and use the matching
Standard/Universal pair. Runtime labels put the version and precision first;
hover an entry to see its filename.

An earlier RTX 40-only kernel pack passed a scoped RTX 4080 SUPER / driver
610.88 game smoke test with two NR passes before upscaling at 3413x960, x4
frame generation and a loading transition. That acceptance belongs to the
earlier binaries. The combined RTX 20/30/40 pack preserves the same RTX 40
cubins, but has separate build and validation evidence. Local RTX 20/30/50
hardware execution, Standard-only gameplay, other-placement visual acceptance
and physical display cadence remain unverified. A tiny 58x32 second-model
stress case times out with both standalone and in-memory paths; use normal
rendering dimensions.

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
Preset C needs DLSS-G 310.9.16 or newer, which Standard 0.4.3 bundles; with
the SDK's 310.9.1 the menu reports that Preset C is unavailable. RTX 20
still requires 310.9.1. RTX 30 uses 310.9.16 by default; set
`RTX30LegacyDLSSG=true` under `[Experimental]` in the user INI to select the
retained 310.9.1 set on the next restart.

## Controls

**End** opens settings. **Apply** changes this session. **Save as default**
also saves settings and window layout; **Discard** drops unapplied edits. DLSS/DLAA mode
and render scale changes take effect after restarting Skyrim.

Saved settings are written to `SKSE/Plugins/TheosRenderPipeline.User.ini`, which
holds only the values you changed. No TRP package contains it (MO2 puts it in
Overwrite), so installing an update keeps your settings; `TheosRenderPipeline.ini`
holds the packaged defaults and updates replace it. Delete a line from the user
file to return that setting to its default. On first start, any custom values in
the winning `TheosRenderPipeline.ini` are imported into the user file once.
Keep your existing INI for that first start; values already overwritten by an
update cannot be recovered. Then use the new packaged INI for future updates.

To change the menu key, open **Advanced**, click the **Menu key** button and
press a new key, then Apply or Save as default. Escape cancels; **Reset to End**
restores the default. Pick a key your other mods and Skyrim controls don't use.
End, function keys, Insert, Page Up/Down, Pause and Scroll Lock also close the
menu while you type in a field; with other keys, click outside the field first.

Live FPS and related measurements stay beside the controls. Resize the window
and drag the column divider to change the layout. The pipeline bar above the
tabs shows the applied rendering order. Advanced contains Lab mode for detailed
runtime information and the information needed for problem reports.

The Neural Rendering tab contains **Base** and named weather overrides. Overrides
change only their edited settings; other settings follow Base. Sharpening is here
too. Passes are folder tabs: **+** adds another pass, up to five; each follows
Pass 1 until you edit one of its settings. **Reset to Pass 1** relinks that pass.
All passes use the same placement; additional passes increase inference work.
**NR per frame** measures preparation, inference or skipped-frame reprojection,
storage, reconstruction and NR UI composition across recent completed frames.
It excludes object capture, the D3D11/D3D12 handoff and frame generation. See
[weather overrides](APPEARANCE-PROFILES.md).

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
- On NVIDIA, XeFG can use the optional tensor model: Frame generation > Tensor
  model, then Save as default and restart. It is off by default and falls back to
  the normal XeFG path when unavailable. Universal installs it in
  `SKSE/Plugins/TheosRenderPipeline/Intel/XeFG-Tensor/` with its notices. Flip
  metering compares driver and software timing for that session only.
- **Advanced → Request loading-screen artwork** defaults on. It asks Skyrim
  to select artwork on eligible cell transitions; startup loading is unchanged.

The renderer includes external ImGui integration. Both included INIs provide
the packaged configuration; `RCAS.hlsl` is the runtime sharpening shader.

## Requirements and reports

The same renderer DLL targets Skyrim 1.6.1170 and experimental 1.5.97/1.6.640/1.7.104.
Install SKSE64 and Address Library matching your game, the x64 Microsoft
Visual C++ runtime and a compatible NVIDIA GPU/driver. DLSS-G-compatible hardware
is required in DLSS/DLAA modes even with frame generation switched off.

AMD and other non-NVIDIA GPUs use experimental XeSS with Intel XeFG; TRP selects
XeSS automatically there, so no INI change is needed. Install the Standard ZIP,
which includes Intel's runtimes and XeFG x2. Universal, installed after it, adds
the experimental XeFG x3-x6 as well as the NVIDIA compatibility paths. With
Community Shaders, CS upscales instead (FSR on AMD) and TRP adds XeFG. Keep AMD Fluid Motion
Frames, Anti-Lag, Radeon Chill, Radeon Boost and other frame-generation or
upscaler mods off. NR requires NVIDIA RTX hardware.

ENB is optional. With Community Shaders, keep CS upscaling enabled. TRP provides
FG/Reflex/NR while CS retains upscaling, render scale, sharpening and colour.
Both editions include these Community Shaders files:

- CS 1.9.1: `SKSE/Plugins/CommunityShaders/Overrides/TheosRenderPipeline_Upscaling.json`.
  This override keeps CS frame generation and Reflex off at startup. If you disable
  it in CS's Overrides tab, turn Frame Generation and Reflex off under CS Upscaling.
- Newer CS / Bottled Shaders: `SKSE/Plugins/CommunityShaders/Presets/Theos Render Pipeline/Theos Render Pipeline.json`
  and `SKSE/Plugins/CommunityShaders/Presets/Theos Render Pipeline/Baseline/TheosRenderPipeline_Upscaling.json`.
  Enable **Theo's Render Pipeline** under **Presets > Baseline** once, then restart
  Skyrim. The pack keeps CS frame generation and Reflex off and leaves CS upscaling
  unchanged. You can instead turn Frame Generation and Reflex off under CS Upscaling
  and restart.

Newer CS ignores the old Overrides file and may report it as an incompatible
old-format override. Both formats are included until CS's next stable release so
CS 1.9.1 still works. On newer CS, installing the files alone does not enable the
Baseline pack. If CS frame generation stays on or is forced, TRP may be unable to
start rendering and Skyrim closes after an explanation. If that prevents you from
opening CS, temporarily disable TRP, start Skyrim and make the CS change, then
close Skyrim, re-enable TRP and restart. TRP does not change CS's settings files.

Bottled Shaders' default post-processing preset turns on Depth of Field, lens flare and colour grading; if near objects look blurred, turn off Depth of Field under Post Processing.

Assign CS a separate menu key,
such as F8, avoiding keys already assigned to ReShade or capture tools. TRP keeps End
unless you change it under Advanced > Menu key.
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

Bottled's new experimental NR implementation is not yet validated alongside
TRP NR. Its runtime can remain loaded even with its NR checkbox off, preventing
TRP NR from starting. Do not switch between the two NR implementations in the
same launch; a restart and isolated runtime ownership are required. This release
does not resolve that conflict.

## ReShade (optional)

ReShade 6.8 was tested with Universal on Skyrim 1.6.1170, Cabbage ENB and
an RTX 4080 SUPER: ordinary 6.8.0.2158 and full add-on 6.8.0.2155, with
early NR and x4. Sky Reflection Fix's ReShade registration was excluded;
Rumble passed a separate initial test. Combined third-party add-ons,
Standard gameplay and CS with ReShade remain unverified. These were earlier
development builds; the exact 0.5.7 package has not been tested with ReShade.

MartysMods ReGrade+ (ReShade 6.6 or newer) used to crash the game while TRP
started ReShade: it prepares itself only in a device event that ReShade does not
send to TRP's ReShade runtime. TRP now sends ReGrade+ that event itself. This was
checked offline with ReGrade+'s build of 2026-03-13 on ReShade 6.8; gameplay is
untested. ReGrade+ grades an 8-bit copy of the frame, so it is meant for SDR;
HDR output with ReGrade+ is untested. If TRP cannot cover ReGrade+, for example
when its add-on file is renamed, TRP leaves ReShade effects and its overlay off
instead of crashing and shows the entry to add under `[ADDON] DisabledAddons` in
ReShade.ini (Advanced → ReShade and the TRP log).

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

### ReShade on the game's device (experimental)

Set `ReShadeGameDevice=true` under `[Compatibility]` in TheosRenderPipeline.ini
and restart. ReShade then starts the way it does without TRP, on the game's own
D3D11 device, so add-ons see the game's device, its draws and one ordinary
Present per frame. ReShade's back buffer is the whole frame, the world with the
HUD over it, so a screenshot that TRP keeps as ReShade saved it (for example
with the overlay open) shows the world as well. ReShade's own menu is still
drawn over the HUD.

This needs ReShade with add-on support. It was tested with the full add-on
build of ReShade 6.8.0.2155 with an add-on loaded, offline and in game on an
RTX 40 card; HDR, Community Shaders and other add-ons are untested with it. If
it cannot start, or its frames keep failing, ReShade's effects and menu turn
off for the session, and the TRP log and **Advanced** say why; set it back to
`false` to use TRP's usual ReShade route.

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
End can conflict with STB Active Effects. Change the menu key under
**Advanced > Menu key**, for example to F10 if free, then Save as default.
Early OAR/IED loading panels remain a known resolution limitation. The test log
retains two Streamline RSYNC errors; physical cadence and unreported menu/cell
transitions remain unverified.
See [PR #33](https://github.com/theosw/theosrenderpipeline/pull/33) for the input fix.

## NR shortcuts

The NR menu checkbox works independently of keyboard shortcuts. Bracket
shortcuts are disabled by default, including for older INIs without the new key.
To opt in, add `EnableNRHotkeys=true` under `[Hotkeys]` in
`TheosRenderPipeline.User.ini` (create it if needed) and restart Skyrim. On a US keyboard, `[` turns NR
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
