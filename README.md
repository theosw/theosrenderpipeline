# Theo's Render Pipeline

Rendering integration for Skyrim: NVIDIA DLSS/DLAA and frame generation,
experimental Intel XeSS upscaling and XeFG frame generation, optional Neural
Rendering (NR), and native-resolution menus and HUD. Current version:
**0.5.3**. See [CHANGELOG.md](CHANGELOG.md) for release changes.

## Features

- DLSS Super Resolution, DLAA, model presets and sharpening.
- Frame generation and multi-frame generation (MFG).
- Experimental Intel XeSS upscaling and XeFG frame generation, including on AMD
  GPUs. See [XeSS and XeFG](#xess-and-xefg-experimental).
- NR before or after DLSS/DLAA, one or two independently configured passes,
  input scaling and tuning, in both editions. NR defaults off.
- Optional peripheral compression, combined NR preparation and an every-other-frame
  NR model rate, all off by default.
- Optional weather overrides for NR appearance settings and sharpening: change
  settings for chosen weathers and places, with smooth transitions and optional
  values by time of day. Overrides are shareable files. See
  [weather overrides](package/APPEARANCE-PROFILES.md).
- ReShade source-frame effects, explicit depth and placement controls.
- Native UI composition, inventory/spell previews, loading artwork and external
  ImGui integration, with live GPU measurements beside the settings.
- Overlay text from external dictionaries, so a language is added or corrected
  without a rebuild. See [localization](#localization).

**End** opens settings; change the key under **Advanced > Menu key**. **Apply**
changes the session; **Save as default** persists settings and window layout;
**Discard** drops unapplied edits. Saved settings go to
`Data/SKSE/Plugins/TheosRenderPipeline.User.ini`, which holds changed values and
explicit overrides needed when an installed `TheosRenderPipeline.ini` has been
customized. No package ships the user
file, so updates keep it; MO2 places it in Overwrite. NR keyboard
shortcuts are opt-in. HDR is experimental, through Community Shaders or TRP's own
HDR output; see [HDR](#hdr-experimental).

NR runs one to five passes; each extra pass processes the previous pass's result
with its own history, and follows Pass 1's settings until you change one of its
own. With two or more NR passes selected, optional **One pass in combat** and
**One pass while weapons/spells are drawn** controls temporarily skip the extra
passes. The return delay defaults to five seconds after all selected conditions
clear and pauses with the game. Your saved pass count, resolution and tuning remain
unchanged. The extra passes stay allocated and their histories reset when they
resume. These options default off; their effect on responsiveness depends on the
workload.

NR's **Model rate** can run the model on **Every other frame**. The frames between
reuse its last change, moved with the game's motion vectors and checked against
depth (and object groups while object rules capture them), which roughly halves
NR's GPU cost. Expect some ghosting in fast motion and on newly revealed areas.
Resets, setting changes and Freeze frame run the model on that frame. It works best with 2x frame generation: at 3x and above the generated frames
between real ones can look unevenly paced. The rate applies to every pass, switches live, is a weather override setting
(`ModelRate`) and defaults to every frame (INI `NRModelRate`).


### Experimental NR object masks

The NR tab can treat first-person arms/weapons, player equipment, other humanoids and creatures independently in each NR pass. Object rules belong to Base rather than to weather overrides and are saved with the ordinary NR preferences. Classification is captured only while a running pass overrides a group or the mask preview is on, and it never skips inference.

Optional group overrides also set tone and structure independently inside each NR pass. Unselected or unavailable classification uses that pass's ordinary intensity/tone/structure values. Tone and structure overrides require a reconstruction NR runtime; legacy NR retains per-pass strength. A pass with active provider controls pauses automatic skin masking for that pass while retaining the saved skin setting. Pass-two object overrides are independent of its “same settings” toggle.

Each group's per-pass **Strength** blends that pass's complete result with its own input, before the next pass. Zero on Pass 2 keeps Pass 1's result; zero on both passes keeps their input. This source-owned blend works independently of the runtime's tone/structure controls. Existing object rules gain strength 1 when their INI has no per-pass strength key. Inference still runs at zero strength. Reduced model grids and peripheral compression use the corresponding guide mapping; temporal and reconstruction filters can spread changes near object boundaries.

There is no whole-chain object strength: zero on both passes already keeps the original object. Per-pass blending adds a model-sized scratch texture and a blend/copy only for passes with an enabled group below strength 1. These textures retire with their NR owner. The NR chain timing includes the per-pass blends and preparation as well as inference.

The session-only mask preview, drawn over the finished NR result, tints the groups you choose to show, or with Classes only shows just those groups' colours on black: green for first-person objects, blue for the player, orange for other humanoids and purple for creatures. Any combination of groups can be shown; showing none turns the preview off. Preview is never saved to the INI and does not reset inference history. UI is composed after the preview, and the same completed scene feeds real frames and frame generation.

Masks default off. The initial capture supports opaque lighting draws on Skyrim 1.6.1170, accepts the original hook entry or another plugin's entry jump (such as SSE Engine Fixes') and chains behind it, and checks capture-frame identity and final depth. Missing, conflicting, transparent, alpha-tested or unclassified capture keeps normal NR behavior. The feature adds geometry replay, classification storage and a composition pass; its game quality, mod compatibility, VRAM use and overhead still need acceptance.

## Localization

The menu's text comes from a language file, so a language is added or corrected
without a rebuild; anything a file does not translate stays English. Pick one with
the language button next to Zoom at the top of the menu, or set `[Overlay] Language`
to `auto` (the Windows display language), `en` or a file's code such as `zh-CN`.
Files go in `SKSE/Plugins/TheosRenderPipeline/Lang`; the package includes
Simplified Chinese. See [LOCALIZATION.md](LOCALIZATION.md) for the file format.

## XeSS and XeFG (experimental)

**Image > Mode > XeSS** selects Intel's upscaler. Apply can exchange DLSS and XeSS at the existing render dimensions when host services and the SDK input range permit it. Quality/render-scale and native DLAA allocation changes require save and restart. A refused live switch preserves the current method and reports that restart is needed.
At startup, Intel's runtime chooses the render resolution for each quality setting. XeSS
keeps the SDR exposure from Skyrim or ENB, so the DLSS model and auto-exposure
settings do not apply. XeSS is available when TRP owns upscaling; with Community
Shaders, CS upscales.

**Frame generation > Provider** chooses NVIDIA DLSS-G or Intel XeFG. **Apply**
switches after the next completed world frame and keeps the upscaling and NR
settings. While XeFG is active, Intel XeLL handles latency instead of Reflex.
On a GPU other than NVIDIA, TRP starts XeFG directly without loading NVIDIA's
runtimes, with XeSS or with Community Shaders' upscaling (FSR on AMD). There,
a saved DLSS mode runs as XeSS at the same quality and DLAA as XeSS Ultra
Quality; **Save as default** then stores XeSS. DLSS, DLAA, DLSS-G and NR
require NVIDIA RTX hardware; on NVIDIA, NR also works with XeSS.

XeFG uses Intel's official x2 by default. With the Universal edition the
multiplier list also offers x3-x6, marked experimental, which patch
`libxess_fg.dll` 1.3.1.78 in memory; no file is changed on disk. Standard ships
Intel's runtimes unmodified and offers x2 only, as it ships NVIDIA's runtimes
without the NVIDIA MFG unlocks. The first Apply to x3-x6 recreates the Intel presenter
without a restart; later changes, including back to x2, apply live. Another
runtime version, or a patch the check
refuses, stays at x2. If the patch state is uncertain, XeFG stops until
the game restarts. **Send frame time to XeFG** (on by default) gives Intel's
pacing the measured frame time, which it uses as a check on non-Intel GPUs.

Switching provider pauses for under a second. On NVIDIA cards XeFG runs as
general GPU work and costs noticeably more than DLSS-G, lowering the rendered
frame rate; DLSS-G remains the better choice there. TRP caches Intel's compiled
shaders in `%LOCALAPPDATA%\TheosRenderPipeline\PipelineCache`; the first
generation after installing or a driver update can stutter briefly while they
compile.

XeFG accepts SDR and HDR10 output, including TRP HDR output. FP16/scRGB output
is not supported; the current presenter is kept instead.

Evidence is one volunteer RX 7900 XT run at 2560x1440 with XeSS and XeFG x2
(88 rendered, 176 output FPS) on an earlier development build. x3-x6 have run
in game only on one RTX 4080 SUPER. Intel Arc GPUs and physical frame cadence
are untested.

## Install

Use SKSE64 and Address Library matching your Skyrim executable. Install packages
are supplied separately; this repository contains source, not the NVIDIA runtime
DLLs. Follow the [Standard installation guide](package/STANDARD-README.md) or
[Universal installation guide](package/README.md).

Standard includes the SR/FG and NR runtime bundle. Universal adds the compatibility
paths and can use Standard's runtimes when installed after it in MO2, or separately
supplied matching runtimes. Both editions retain NR. Enable only one winning
renderer DLL. NVIDIA modes initialize the NVIDIA host even when interpolation
is off; XeSS uses the Intel startup path. Standard includes Intel's XeSS, XeFG
and XeLL runtimes in `Data/SKSE/Plugins/TheosRenderPipeline/Intel`, with Intel's
license in `licenses/Intel-XeSS-LICENSE.txt`.

The DLL selects Community Shaders integration when CommunityShaders.dll is loaded;
otherwise TRP owns upscaling, with optional ENB. With CS, both editions include a
CS settings override that keeps CS frame generation and Reflex off; CS retains
its shading, upscaling, UI and HDR output. CS builds without an Overrides tab
need both turned off by hand.
Keep SSE ReShade Helper disabled when using TRP's ReShade integration. Use one shading
setup per profile and disable competing upscaler/frame-generation injectors.

## ReShade tested scope

ReShade 6.8 was tested with Universal on Skyrim 1.6.1170, Cabbage ENB and
an RTX 4080 SUPER: ordinary 6.8.0.2158 and full add-on 6.8.0.2155, with
early NR and x4. Sky Reflection Fix's ReShade registration was excluded;
Rumble passed a separate initial test. ReGrade+, combined third-party add-ons,
Standard gameplay and CS with ReShade remain unverified. These were earlier
development builds; the exact 0.4.0 package still needs its final game test.

## HDR (experimental)

With Community Shaders, HDR uses its HDR Display (tested with CS 1.9.1 and
HDR Display 1.2.2). CS composes the HDR10 image; TRP adds frame generation and NR.
Enable Windows HDR and use borderless windowed. In CS's HDR Display settings, set
peak brightness to your monitor's value and start paper white near the Windows SDR
content brightness, then adjust to taste.

Weathers and lighting built for ENB, such as NAT.ENB, need CS Effects 11 with a
preset; without one the image is much darker in both SDR and HDR. Two
full-resolution NR passes after upscaling are expensive; prefer one pass or NR
before upscaling. HDR through Special K, RenoDX or Linear Lighting is not
supported, and most ReShade effects are not HDR-aware.

Evidence is one Universal RTX 4080 SUPER LoreRim UltraCS setup with Effects 11:
x4 generation, both NR placements and HDR on/off switching. Standard, other
hardware and physical frame cadence remain untested.

### TRP HDR output (ENB and non-CS setups)

Without Community Shaders, **Image > HDR output** expands the finished SDR image,
including ENB's, to HDR10. This is inverse tone mapping: SDR white is shown at
paper white, the brightest areas are expanded towards peak brightness, and the
native UI, menus and TRP overlay use their own brightness. Highlights the preset
already clipped cannot be recovered, and 8-bit output can band in bright gradients.
Frame generation receives matching HDR10 HUD-less and UI images. NR and ReShade
still run on the SDR image.

Enable Windows HDR, turn on **HDR output** and apply; save as default to keep it.
The first switch on in a session reallocates the swapchain and briefly pauses
frame generation. Turning it off keeps the 10-bit swapchain until restart. By
default, paper white and UI brightness follow Windows' SDR content brightness
(**Match Windows SDR brightness**). Paper white, peak, UI brightness, highlight
strength, expansion start and SDR decoding apply live. When Windows HDR is off for the game's display, output stays SDR.
Leave ENB's own HDR-like effects as they are; this does not change the preset.
The panel shows the output pass's GPU time.

Evidence is one Universal RTX 4080 SUPER LoreRim ENB setup at 5120x1440 on a
1015-nit display: x4 generation, NR before upscaling, a loading door and live
calibration. That run logged more NVIDIA "flip queue is empty" messages than a
matched HDR-off run, without a visible hitch. Recurring vendor flip-queue errors
remain unresolved. Loading screens forced by **Request loading artwork** now
fade in from black. Later ENB checks also cover Wheeler and HUD/End over menus.
Standard, other hardware and physical frame cadence remain untested.

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

## GPU failure reports

Device removal and stalled GPU work write a bounded `[GPUFailure]` report to
`TheosRenderPipeline.log`, including the original operation, device-removal
reasons and retained fence/slot state. Startup and configuration failures keep
their own error line. This reports the failure; restarting the game remains necessary.
For a requested GPU-fault investigation, create
`Data/SKSE/Plugins/TheosRenderPipeline.Diagnostics.ini` with:

```ini
[DeviceLoss]
EnableDRED=true
```

Restart after changing this diagnostic setting. DRED enables additional GPU
tracking for subsequently created D3D12 devices in the process and can add
overhead; leave the file absent during normal play. An absent or false setting
leaves Windows and externally configured DRED settings unchanged. Empty DRED
data does not exclude a GPU fault, and breadcrumbs do not identify a cause alone.

## Build

Requires Windows, Visual Studio 2022 C++ tools/Windows SDK, CMake and vcpkg.
Use CommonLibSSE-NG 8.1.0 at `3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd`,
Streamline 2.11.1 public headers and NGX SDK headers/import library, the
Intel XeSS SDK at `de0fb9c1c510661c571164e1418ceca8101dab69` (XeSS SR, XeFG 1.3
and XeLL 1.3 headers), and Nukem9/detours at
`cc5a2e4a58ef462821877b35ad30215f0e16bba1`. CommonLib's
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
  "-DARP_DETOURS_DIR=C:/deps/detours" `
  "-DTRP_XEFG_SDK_DIR=C:/deps/xess"
cmake --build --preset arp-nvidia --parallel 2
```

`ARPBaseline` builds the renderer in `out/build/nvidia/Release` without deploying
or launching. The build compiles and embeds NR shader bytecode; generated headers
stay in the build directory. Configure separate directories for Standard
(`TRP_ENABLE_OPTIONAL_FEATURES=OFF`) and Universal (`ON`, default), keeping
`TRP_ENABLE_NEURAL_RENDERING=ON` for both. Store workstation paths in ignored
`CMakeUserPresets.json`. Set `TRP_NGX_LIB` if the NGX import library is elsewhere.

Alternatively, set two environment variables: `VCPKG_ROOT` to your vcpkg
installation, and `TRP_DEPENDENCY_ROOT` to a directory containing `CommonLibSSE-NG/`,
`Streamline/`, `detours/`, `hde64/`, `xess/` and a prepared `vcpkg_installed/` tree. The
`trp-standard` and `trp-universal` presets then configure each edition with the
standalone checks enabled from any checkout or worktree, without a local preset
file. They stay hidden until both variables are set, and the generated build
re-runs CMake automatically after a CMake input changes.

Enable `TRP_BUILD_COMPATIBILITY_TESTS=ON` for the standalone checks, build all
targets with `cmake --build <build-directory> --config Release`, then run
`ctest --test-dir <build-directory> -C Release --output-on-failure`. These checks
do not establish game acceptance. Build output alone is not a complete install;
use the installation guides for configuration, shaders and required runtimes.

The Intel fixtures in `tests/xefg` are built and run explicitly against
identified Intel and NVIDIA runtime directories. Their output counts do not
establish physical display cadence.

The same build includes `TRPOverlayPreview`, which draws the in-game menu
offline from sample renderer states and writes PNGs for reviewing menu
changes without launching Skyrim. See [the preview guide](tools/OverlayPreview/README.md).

The optional Universal XeFG tensor helper has a separate, opt-in
[source build](src/FrameGen/XeFGExtension/README.md). Its maintained C++ is in
`src/FrameGen/XeFGExtension`; compiled GPU assets and generated runtime profiles
are supplied separately and verified against a tracked manifest. The helper is
not required to compile the normal renderer and does not deploy automatically.

The optional ReShade lifecycle fixture compares automatic and exported D3D11
runtimes, single effect execution, effects-off GUI completion, shared input and
paired teardown. It needs a supplied x64 ReShade DLL and an accepted hardware
adapter; no DLL is downloaded, bundled or installed. Configure with
`-DTRP_RESHADE_TEST_RUNTIME=C:/path/to/dxgi.dll`, build
`TRPReShadeLifecycleTests`, then run `ctest -R ReShadeLifecycleRuntime` in that
build directory with `-C Release --output-on-failure`. Each run copies the DLL
and fixture into fresh folders under the build directory and records its hash.
Logs and configurations are retained; disposable executable/DLL copies are removed
after successful cases. The runner's `--keep-binaries` option retains them too.
Children run in a kill-on-close Windows job so terminating the runner retires them.
`TRP_RESHADE_TEST_INPUT_POLICY` defaults to `observe`; use `every-runtime` for
6.3.3 or `first-runtime` for 6.8.0 to enforce the version-specific input check.
Observation records both runtimes' raw key states without imposing either policy.
These are upstream runtime contracts, not a ReGrade+ reproduction, race test,
performance benchmark or Skyrim compatibility guarantee. The fixture can also
be built independently with `cmake -S tests/reshade-lifecycle -B out/reshade`.

See [LICENSE](LICENSE) and [third-party notices](THIRD-PARTY.md) for project,
dependency and contribution terms. Vendor runtimes retain their own licenses.
