# Unreleased

- Fix the mouse cursor disappearing in the menu at 75% zoom. ImGui rounds the
  cursor's scale down to a whole number, which was 0 below 100%; the cursor now
  follows the zoom and is never smaller than normal.

# 0.5.3

Game-tested on an RTX 4080 SUPER with DLSS-G and XeFG frame generation: the Simplified
Chinese menu and Save as default, and PrintScreen at 5120x1440.

- The menu can be translated without rebuilding the DLL. Its text is read from a
  language file, `SKSE/Plugins/TheosRenderPipeline/Lang/<code>.ini`; anything a
  file does not translate stays English. Simplified Chinese ships as `zh-CN.ini`.
  Choose the language with the button next to Zoom at the top of the menu, which
  lists each language under its own name; it applies at once and **Save as
  default** keeps it (`[Overlay] Language`: `auto` follows the Windows display
  language, `en` keeps English). Settings files, weather overrides and the log
  stay English. A translation whose `%` placeholders differ from the English
  keeps the English, so a typo cannot crash the game. `LOCALIZATION.md` describes
  the file format and `tools/lang/audit.py` lists what a file still lacks. Thanks
  to lin-414, who built it (#116).
- Skyrim's own screenshots (PrintScreen) now contain TRP's finished frame at the
  output resolution, with DLSS and NR, from a real rather than generated frame,
  instead of the game's render-size image. Skyrim's file is written as before and
  then replaced from the same frame; if that fails, Skyrim's file stays. A visible
  HUD is included, as on screen. Photo Mode saves its photos itself and is not
  covered yet: they stay at the render size.
- Steady frame limit: a faster cap that does not hold now returns to the cap that
  held, and the next try waits twice as long (up to about a minute). Before, it
  dropped 3% below the working cap and tried again every few seconds.
- NR Model rate no longer recommends the Steady frame limit. With Every other
  frame, Steady capped below what the game could manage without evening out the
  frames; Off played better in testing.

# 0.5.2

Game-tested on an RTX 4080 SUPER with XeFG and DLSS-G frame generation, including
NR Model rate's Every other frame with both.

- NR runs up to five passes. **+** on the pass strip adds the next one; each extra
  pass processes the previous pass's result with its own history, input
  resolution, network, tuning and object rules, and follows Pass 1 until one of
  its own settings changes. New keys follow Pass 2's (`NRPass3InputScale`,
  `NRObjectPass4Group1Enabled`, weather override `Pass5Style` and so on); Pass 2's
  keys are unchanged. Passes 3 to 5 start one per frame. If NVIDIA's runtime
  refuses to start one of them, the passes before it keep running and the NR tab
  says which pass could not start.
- The pass strip's **Reset all to Pass 1** is now **Reset to Pass 1**: it relinks
  only the selected pass and keeps its own settings for later.
- Frame generation > Frame limit replaces Output FPS limit: Off, Fixed (the output
  cap, as before) or Steady, which measures recent rendered frame times, holds
  every frame to a cap the frames can sustain on average, so frames arrive evenly,
  and every few seconds tries a slightly faster one. The menu shows the cap in use.
  A saved `OutputFPSLimit` loads as Fixed (INI `FrameLimit`).
- Add NR Model rate. Every other frame runs NR's model on alternate frames and
  carries its change to the frames between with motion vectors and depth, roughly
  halving NR's GPU cost, with some ghosting in fast motion and on newly revealed
  areas. It applies to every pass, switches live and is a weather override setting
  (`ModelRate`; INI `NRModelRate`). The NR tab recommends the Steady frame limit
  with it. It works best with 2x frame generation: at 3x and above the generated
  frames between real ones can look unevenly paced. Thanks to cavy8, whose
  Alternate Frames for Community Shaders NR showed the approach.
- Native-resolution drawing for ImGui mods now also finds builds it has no exact
  profile for (newer Photo Mode 3.x, dMenu, Detection Meter and SpellHotbar
  variants) by their code, and installs only when exactly one match is found.
  Unprofiled Vanity builds get native drawing without their build-specific mouse
  and text fixes. `DiscoverUnknownBuilds=0` under `[Adapters]` in
  `TheosRenderPipelineImGui.ini` turns this off.
- Fix NR after upscaling and frame generation's UI handling stopping while the game
  hides its interface (tm, the console, Photo Mode's hide and screenshots, or any
  mod that hides menus).
- Fix the vanilla main menu disappearing after its first frame, leaving only the
  3D background, with `NativeUICompositionMode=0`.
- Fix a possible presentation freeze when the first gameplay camera arrived with
  XeFG on native Skyrim (the end of the New Game intro or the first loaded
  frame). XeFG now starts with Skyrim's forward depth, so it no longer needs
  rebuilding at that point, even with generation off.

# 0.5.1

Game-tested on an RTX 4080 SUPER with ENB. The Community Shaders changes (the grey
guard on its colour path and the steady grid before upscaling) passed GPU tests
but have not been tested in game.

- Add Neural Rendering > Compare, a live, session-only comparison that is never
  saved. Split shows the frame without NR left of a movable line. Freeze frame
  (after upscaling only) keeps NR working on one held frame while the game runs
  on, so setting changes compare on identical content.
- Add NR Exposure for Ratio reconstruction of linear HDR input: Fixed keeps the
  white point; Adaptive measures the frame's average brightness and adapts over
  about a second, with a bias in EV, so NR sees interiors and exteriors at a
  similar brightness. Only NR's view changes. Both are weather override settings
  (`AdaptiveExposure`, `ExposureBias`; INI `NRAdaptiveExposure`, `NRExposureBias`).
- Before upscaling, Ratio reconstruction now shows NR the frame on the steady,
  unjittered grid and moves its change back to each jittered pixel. DLSS still
  receives the jittered frame, on TRP's own route and with Community Shaders.
  Auto and Residual are unchanged.
- Keep greys grey: where the original is near neutral, NR may change saturation
  along its own hue but not tint it, which removes NR's green cast on grey
  shading (Ratio and the Community Shaders route).
- Ratio reconstruction treats a black answer from NR as a real shadow. Only an
  entirely empty model output keeps the original, so shadows NR paints over lit
  surfaces no longer leave bright specks.
- Add NR Broad and Detail for Ratio reconstruction: Broad scales how much NR
  relights whole areas and objects (lower it if object shadows change as the view
  changes), Detail its local contrast and shading. Both default to 1, which leaves
  the result unchanged, and both are weather override settings (`BroadStrength`,
  `DetailStrength`; INI `NRBroadStrength`, `NRDetailStrength`).
- The XeFG tensor model now switches live: Apply recreates the XeFG presenter on
  the other Intel SDK copy after a short pause, instead of needing a restart.
  Both copies stay loaded and share one XeLL, and each keeps its own pipeline
  cache.
- Once a running XeFG presenter has tried x3 to x6 and Intel refused or capped
  them, the Multiplier row greys the counts it cannot reach, with the reason.
- NR settings that rebuild the model (placement, reconstruction, network, input)
  no longer re-verify the 266 MB runtime each time, which froze the game for
  several seconds. The log records each rebuild step's time.
- Build the renderer with whole-program optimization, which its link-time code
  generation already expected.

# 0.5.0

Settings note: object rules from 0.4.5 (`NRObjectMasks` and the `NR*Strength`
keys) are not carried over. Set them again in each NR pass.

- Rework the settings menu. Neural Rendering shows passes as folder tabs over one
  pass's settings, with Reset all to Pass 1 on the Pass 2 tab, a small undo arrow
  on changed rows and object rules inside each pass (an eye per group for the mask
  preview). Short pick-one choices are segmented controls; choices that combine
  stay checkboxes. Frame generation is one list with the provider at the top.
  Settings that do not apply are greyed out with the reason, never hidden.
- Rename presets to weather overrides. Conditions are rows: exact weathers, weather
  types, places (Interiors, Anywhere outside) and a time-of-day strip with All day
  first. While Base is selected, a cloud marks settings the current override
  replaces. Files, the `Presets` folder and INI keys are unchanged.
- NR Style is Default, Natural, Cinematic or Custom; clicking Custom edits any
  number from 3 to 255 in place. Saved styles load within 0 to 255.
- NR Network is Small (310.9.16's faster model, the default) or Old (the model 310.8
  runs). 310.9.16's Old output matches 310.8 exactly.
- Object rules can be switched on before the capture hook installs; the menu
  previously greyed them on Skyrim 1.6.1170 with a misleading version message.
- Fix a colour-stack imbalance when ticking a weather type another override uses.
- Add `TRPOverlayPreview`, which draws the menu offline to PNGs or in a live window
  with scripted clicks, typing and saves; a CTest renders every scenario and tab.
- Add an optional XeFG tensor model for NVIDIA GPUs in Universal: Frame generation
  > Tensor model, off by default; Save as default and restart to change it.
  Universal includes the helper and the Intel XeFG runtime it uses in
  `SKSE/Plugins/TheosRenderPipeline/Intel/XeFG-Tensor/`, with their notices. When
  the bundle or the model is unavailable, XeFG uses its normal path.
- Add session-only NVIDIA flip metering for XeFG with the tensor model. It
  compares driver and software timing; the driver must accept it.
- Add VSync Auto, Off and On under Frame generation. Auto turns VSync off for
  available XeFG driver metering and otherwise follows the game; On prevents
  metering. Apply changes it live; Save as default keeps it.
- Add XeFG scene change sensitivity: how readily a sudden change, such as a camera
  cut or a fast turn, counts as a new scene. Intel's default is 0.70.
- Save as default keeps explicitly selected settings when the installed base INI
  was customized; NVIDIA could previously be dropped from the saved provider.
- XeFG receives SDR inputs in a compatible format after HDR output is turned off.

- Give every object group independent Strength, Tone and Structure controls in
  Pass 1 and Pass 2. Strength blends with that pass's input; zero on Pass 2
  retains Pass 1's work. Both model evaluations still run.
- Remove whole-chain object protection and the Object-specific NR switch. Object
  rules now live only in each pass; capture runs while a running pass overrides
  a group or the mask preview is on. The released `NRObjectMasks` and
  `NR*Strength` keys are no longer read or written.
- Add a session-only NR object-mask preview that shows any combination of
  groups as a coloured overlay, or as classes only.
- Add independent per-group tone/structure overrides inside each NR pass. Active
  controls pause automatic skin masking for that pass; saved skin settings remain
  unchanged. Legacy NR retains per-pass strength. Object capture remains opt-in
  and uses the existing opaque-draw replay path.

# 0.4.5

- Reduce repeated NR recording work: use constant parameter names, reuse
  resolve descriptor tables while their resources stay the same, batch resource
  transitions and read the second pass's result directly when a resolve follows.
  Model inference, controls and settings defaults are unchanged.
- Keep NR before upscaling active on world frames without frame-generation UI
  guides, including hidden-interface and Present fallback frames. Main menus
  and loading screens still suspend NR; after-upscaling composition keeps its
  UI requirements. Thanks to Eragon327 for the original fix (#104).
- Check the NVIDIA driver before preparing RTX 20/30 frame-generation
  compatibility. Drivers older than 570.65 stop with update instructions;
  572.16 or newer is recommended. An unreadable version is logged and remains
  admitted to the existing runtime checks.
- Stop repeated vendor FG creation attempts after the first failed RTX 20/30
  feature creation. Preserve its original error, retain potentially in-flight
  resources, and require a restart. Successful feature recreation is unchanged.
- Automatic NR can select an available compatible runtime when the legacy DLL
  is missing. Startup, Apply and the NR enable control use the same file and
  GPU policy. Explicit custom paths remain explicit.
- Run MartysMods ReGrade+ with TRP's ReShade effects. ReGrade+ (ReShade 6.6 or
  newer) prepares itself only in a device event that ReShade does not send to
  TRP's ReShade runtime, so starting ReShade crashed the game. TRP now sends
  ReGrade+ that event for its own runtime only. If it cannot, TRP turns its
  ReShade effects off and names the `DisabledAddons` entry instead of crashing.
- Correct the installation notes for RTX 30's default DLSS-G 310.9.16 route
  and document runtime selection when upgrading.
- Write DLSS and sharpening output directly into the output texture by
  default, skipping a copy. Each route falls back to the copy for the session
  if the texture does not qualify; `DirectRCASOutput`/`DirectDLSSOutput=false`
  turn them off.
- Restart the rolling timing window when the frame-generation provider,
  preset, multiplier or dynamic mode changes, so each configuration's
  percentiles and frame-generation time stand alone.
- Lab A/B (developer controls): let the game's next frame render while NVIDIA
  frame generation still reads the current frame's inputs. Off by default.
- Switch between DLSS and XeSS with Apply at the current render size when the
  SDK accepts it and NVIDIA's services are ready. Render scale and DLAA changes
  still need a restart; a refused switch keeps the current mode and says so.
- Experimental NR object masks, off by default: set how much NR applies to
  first-person arms and weapons, player equipment, other humanoids and
  creatures, from 0 (original) to 1 (full NR). Opaque objects only on Skyrim
  1.6.1170, alongside SSE Engine Fixes, which patches the same draw function;
  anything it cannot classify keeps normal NR. It does not reduce NR's cost.

Apart from direct output, packaged settings defaults are unchanged, as are the
Standard/Universal runtime split, NR tuning, placements and passes. Tested in
game on an RTX 4080 SUPER with LoreRim and ENB: NR with one and two passes, x4
frame generation, HDR, live DLSS/XeSS switching, hidden UI, loading screens,
ReGrade+, ReShade screenshots and object masks. RTX 20/30/50 and physical
display cadence remain unverified.

# 0.4.4

- RTX 30 Universal now uses DLSS-G 310.9.16 by default, including Preset C.
  `RTX30LegacyDLSSG=true` retains the older 310.9.1 route. RTX 20 continues
  using 310.9.1.
- Universal runs original NVIDIA Neural Rendering 310.9.16 on RTX 20/30/40
  using a verified kernel pack installed in memory. The original DLL stays
  unchanged on disk. RTX 50 continues using NVIDIA's original kernels.
- Standard retains two NR DLLs: legacy 310.8 and original 310.9.16. Universal
  supplies the compatibility pack and retains the legacy community INT8 v2
  DLL for RTX 20/30. Duplicate full community and standalone RTX 40 310.9.16
  DLLs are replaced by the pack. Install matching Standard first, then
  Universal after it in MO2.
- Automatic prefers 310.9.16 on supported GPUs. Safe refusal falls back to
  legacy INT8 then 310.8 on RTX 20/30, or 310.8 on RTX 40/50. INT8's known-bad
  RTX 40 path remains blocked. Standard alone keeps 310.8 on older GPUs.
- Existing explicit custom runtime choices are kept; older default-runtime
  settings migrate to Automatic. Runtime labels put version and precision
  first, with filenames available on hover.
- NR reports GPU time separately for inference, preparation, reconstruction/UI
  and the D3D11/D3D12 handoff, helping identify which stages cost frame time.

Both editions retain NR, existing frame-generation compatibility and packaged
settings defaults. Earlier RTX 40-only builds received scoped RTX 4080 SUPER
game acceptance for two NR passes before upscaling, x4 frame generation and a
loading transition. The combined pack retains those SM89 cubins but has a new
identity; that earlier acceptance does not establish gameplay acceptance for
the rebuilt packages. Local RTX 20/30/50 execution, Standard-only gameplay and
physical cadence remain unverified. The tiny 58x32 second-model stress case
remains a shared limitation of standalone and in-memory paths.

# 0.4.3

- Add a **Frame generation preset** dropdown with NVIDIA's Presets A
  (standard), B (UI recomposition, the previous default) and C (Lite blending,
  beta). Changes apply live. A saved UI recomposition setting carries over: on
  becomes B, off becomes A. If the NVIDIA app or driver profile forces a
  preset, the menu shows it; NVIDIA's runtime gives that preset priority. XeFG
  keeps its UI recomposition checkbox.
- Standard now bundles DLSS Frame Generation 310.9.16, which adds Preset C.
  Experimental RTX 20/30 frame generation still requires 310.9.1, so Standard
  also includes that Streamline set in `NVIDIA/Streamline-RTX20-30/`, and TRP
  selects it automatically on those cards. This switch has not yet been tested
  on RTX 20/30 hardware.
- Add an NR runtime selector to the NR tab. It lists every
  `nvngx_dlssnr*.dll` in the `NVIDIA` folder, so newer NVIDIA builds and
  community builds can be chosen without renaming files. Runtimes load by
  version (310 or newer). The community int8 v2 build renders black on RTX 40
  cards, so there TRP uses NVIDIA's `nvngx_dlssnr.dll` instead and says so in
  the NR tab; RTX 20, 30 and 50 cards can still use it.
- Fix flickering faces and lighting in dialogue with NR on, a 0.4.2
  regression. The underwater motion repair now runs only while the game reports
  the player underwater.
- On RTX 40 cards, a Frame Generation override from the NVIDIA app no longer
  stops Skyrim at startup. If x3-x6 cannot be enabled for that DLSS-G build,
  frame generation continues at x2.
- Fix missing parts of the UI layer when another mod sets blending before the
  UI pass.
- Saved settings and presets are written safely, so a failed write can no
  longer leave them empty or truncated.
- TRP's mip LOD bias now applies only to the game's own rendering, not other
  mods' rendering contexts.
- XeFG no longer interpolates a stale HDR world image.
- Logs report misrouted motion or depth inputs, NGX release failures and
  underwater motion repair decisions.

Settings files carry over. `FrameGenerationPreset` replaces
`UIRecomposition`, which is still written for older versions.

# 0.4.2

- Fix an offset copy of the scene, and ghosting of bright lights while moving,
  in generated frames while the camera is underwater. Skyrim writes no real
  motion there; when nearly the whole frame disagrees with the camera's own
  motion, frame generation and NR now use the camera's motion instead. This is
  on by default on the vanilla and ENB routes. With **Advanced → Lab mode** on,
  **Frame generation → Repair missing motion** turns it off until restart.
  Community Shaders is unchanged.
- Lab mode adds two Frame generation views: **Show only generated frames** for
  DLSS-G (XeFG already had it), and **Show frame generation inputs**, which
  displays the motion vectors, depth and camera-motion disagreement that frame
  generation receives.

Settings files, vendor runtimes and packaged defaults are unchanged from 0.4.1.

# 0.4.1

- Keep experimental Intel XeFG x3-x6 in Universal only. Standard retains XeSS
  and official XeFG x2; a saved higher multiplier runs at x2 and is retained
  for Universal. Standard's multiplier list explains the edition requirement.
- Correct alpha coverage for source-over and additive draws into TRP's separate
  UI layer, including legacy ImGui producers, while preserving their RGB blend
  equations and restoring their blend state when leaving the layer.
- Save settings and menu layout to `TheosRenderPipeline.User.ini`, which is
  never shipped in update packages. Import custom values from a winning legacy
  INI once, preserving them through subsequent saves and updates. See the
  installation guide for the first-update migration steps.
- Add likely causes and suggested fixes to common rendering and MFG startup
  errors, alongside the original technical error and log details.

Both editions retain NR and their existing frame-generation routes. Vendor
runtime files and packaged settings defaults are unchanged from 0.4.0.

# 0.4.0

- Add experimental Intel XeSS upscaling under Image > Mode > XeSS; save and
  restart to apply it. Intel's runtime chooses the render resolution for each
  quality level.
- Add experimental Intel XeFG frame generation under Frame generation > Provider.
  Apply switches between NVIDIA DLSS-G and XeFG after the next world frame.
  Intel XeLL replaces Reflex while XeFG is active. SDR and HDR10 output are
  supported, and the HUD is composed from the UI layer as with DLSS-G.
- XeFG offers Intel's official x2 and experimental x3-x6 in the multiplier list.
  x3-x6 patch `libxess_fg.dll` 1.3.1.78 in memory after strict checks; another
  runtime version stays at x2. The first Apply to x3-x6 recreates the Intel
  presenter; later multiplier changes apply live. **Send frame time to XeFG** is
  on by default.
- Switching provider pauses for under a second. After a switch or loading
  screen, XeFG waits for frame times to settle before it starts generating, so
  a slow start does not throw off Intel's frame pacing. TRP caches Intel's
  compiled shaders in `%LOCALAPPDATA%\TheosRenderPipeline\PipelineCache` to
  shorten the stutter when XeFG starts generating. On NVIDIA cards XeFG costs noticeably
  more GPU time than DLSS-G, which remains the better choice there.
- AMD and other non-NVIDIA GPUs run XeSS and XeFG automatically, without
  NVIDIA's runtimes or an INI change. With Community Shaders, CS upscales (FSR
  on AMD) and TRP adds XeFG. NR still requires NVIDIA RTX hardware.
- Standard includes Intel's XeSS, XeFG and XeLL runtimes with Intel's license.
- Both editions include a Community Shaders settings override that keeps CS
  frame generation and CS Reflex off. CS builds without an Overrides tab still
  need both turned off by hand.
- In an XeSS session on NVIDIA where NR has started, a live switch to DLSS-G is
  refused instead of closing Skyrim; save the NVIDIA provider and restart.

Both editions retain NR and the existing NVIDIA frame-generation routes. NVIDIA
runtime DLLs and NVIDIA defaults are unchanged from 0.3.7. XeSS, XeFG, HDR and
RTX 20 compatibility are experimental. XeSS and XeFG have limited AMD testing
and none on Intel Arc. This release does not resolve the conflict with Bottled's
experimental NR implementation.

# 0.3.7

- Rebind the settings-menu key under Advanced > Menu key. Apply switches the
  key immediately; Save as default remembers it. Escape cancels picking, and
  Reset to End restores the default. Function keys and supported navigation
  keys can close the menu while editing; letter and number bindings keep text
  editing active until the field loses focus.
- Switch TRP's experimental HDR output on or off with Apply, without restarting.
  The first activation in a session briefly pauses frame generation while the
  native output buffers become 10-bit. Turning HDR off keeps those buffers until
  restart; subsequent switches change the output colour space immediately.
  Windows HDR is required. Community Shaders continues to own HDR on its route.

Both editions retain NR and existing frame-generation compatibility routes.
NVIDIA runtime DLLs and packaged feature defaults are unchanged from 0.3.6.
HDR and RTX 20 compatibility remain experimental. This release does not resolve
the conflict with Bottled's experimental NR implementation.

# 0.3.6

- Scale the menu automatically for higher output resolutions, with a bundled
  Roboto font for sharp text. No separately installed font is required.
- Add title-bar Zoom controls, automatic reset and an Advanced zoom slider.
  Save as default remembers zoom, window dimensions and the column divider.
- Scale the graphs, pipeline diagram, settings columns and preset popups together.
- Improve GPU failure diagnostics: retain the first fault and report it once,
  with optional DRED breadcrumb evidence. DRED remains off by default; existing
  GPU progress waits and resource-retention protections are preserved.

Both editions retain NR and existing frame-generation compatibility routes.
NVIDIA runtime DLLs are unchanged. HDR and RTX 20 compatibility remain
experimental. Compatibility between TRP NR and Bottled's new experimental NR
implementation remains unverified; loading its NR runtime can prevent TRP NR
from starting. This release does not fix that conflict.

# 0.3.5

- Reduce HUD ghosting in generated frames: DLSS-G now interpolates the HUD-less
  scene and the UI separately (UI recomposition). On by default, including for
  existing INIs; toggle it live under Frame generation. Small GPU/VRAM cost.
- Fixed a black screen that could persist after a cell load while the game kept
  running: a multi-second GPU stall no longer counts as a fatal fault. TRP keeps
  waiting while the GPU makes progress and logs long waits. A fence with no
  progress still fails after 20 seconds; device removal remains a failure.
- Expanded automated ReShade runtime ownership, input and lifecycle tests.

Ordinary and full add-on ReShade 6.8 have scoped Universal/ENB gameplay
evidence. ReGrade+ and some third-party add-on combinations remain unverified;
this release does not add a runtime fix for those combinations. NVIDIA
runtimes are unchanged. Both editions retain NR and existing compatibility
routes; HDR remains experimental and off by default.

# 0.3.4

- Added experimental TRP HDR output for ENB and other non-CS setups: expand the
  finished SDR image to HDR10 with paper white, peak, UI brightness and highlight
  controls. Paper white and UI follow Windows SDR content brightness by default.
  Off by default; enabling it needs Save as default and a restart.
- Keep late overlays visible over opaque UI in HDR, with matching frame-generation
  UI/HUD-less layers and a separate UI brightness.
- Fixed GPU device loss with Modex 3D item previews under DLSS by routing their
  copies and clears to the native UI target while Modex draws.
- Drop and log copies that fall outside the game-facing/native UI textures, and
  include D3D11/D3D12 removal reasons in device-loss diagnostics.
- Fade forced loading artwork in from black on door transitions, avoiding a brief
  bright flash.

Community Shaders HDR Display integration is retained. HDR remains experimental;
ENB acceptance covers Universal on one RTX 4080 SUPER with x4, early NR, Wheeler,
HUD/End over menus and loading recovery. Standard HDR, other hardware and physical
frame cadence remain unverified. Recurring NVIDIA flip-queue errors are unresolved.
Both editions retain NR and existing compatibility routes. NVIDIA runtimes are
unchanged; TRP HDR output defaults off. See the README HDR notes for setup.

# 0.3.3

- Added experimental HDR with Community Shaders HDR Display. TRP identifies the
  UI before CS composes HDR, so the menu stays visible and frame generation runs.
- Frame generation receives CS's own HDR10 conversion of the HUD-less scene.
- NR after upscaling keeps HDR highlights and its normal look on CS's HDR scene.
- D3D11 capture hooks now survive runtime state changes.
- ReShade screenshots skip already-encoded HDR output instead of miscolouring it.
- Fixed camera capture dereferencing stale cached camera pointers by selecting
  and retaining a live camera.
- Fixed the optional direct DLSS and direct sharpening output routes on eligible
  textures, with allocation fallback and clearer failure diagnostics. Both
  options remain off by default.

HDR requires CS HDR Display; ENB-style weathers also need CS Effects 11 with a
preset. See the README's HDR notes for setup and tested scope.
NVIDIA runtime DLLs and configuration defaults are unchanged. Both editions
retain NR and their existing frame-generation capabilities.

# 0.3.2

- Added weather and time presets for individual NR settings and sharpening,
  with Base inheritance, interior/weather assignments and per-setting reset.
- Added shareable preset INI files, list priority, and full preset copies.
- Moved presets and sharpening into the Neural Rendering tab alongside the
  Pass 1 / Pass 2 controls; added keyboard-layout-aware text entry.
- Added optional one-pass NR during combat or while weapons/spells are drawn,
  with a configurable delay before the second pass resumes.
- Corrected ReShade screenshots using a subsequent completed frame containing
  DLSS, NR and the HUD; the existing capture limitations remain documented.

Both editions retain NR and their existing frame-generation capabilities.
NVIDIA runtime files are unchanged. HDR remains unsupported. Preset files use
the new format and are not readable by earlier builds; preserve settings before
switching back to an older version.

# 0.3.1

- Allow frame generation and Neural Rendering during RaceMenu character creation,
  while retaining the loading-screen and fade guards.
- Reduce NR activation work by embedding its shaders at build time and reusing
  the verified runtime identity when toggling a retained NR feature. First-time
  NVIDIA model and pipeline creation can still cause a pause.
- Support Bottled Shaders' reverse-Z depth format and forward the correct depth
  convention to frame generation and both NR placements.
- Correct GPU timing scopes and the labels for rendering-stage measurements.

NVIDIA runtime DLLs and configuration defaults are unchanged. HDR remains
unsupported, and RTX 20/30 compatibility retains the documented test limits.

# 0.3.0

- Added experimental RTX 20 frame-generation compatibility in Universal.
- Select compatible PTX endpoint networks on Turing instead of incompatible
  precompiled kernels, with runtime fingerprints and preparation checks.
- Stop clearly at the first real RTX 20 kernel-loading failure and identify the
  rejected program, preserving the runtime's API-presence probes.
- Retained all released 0.2.5 startup, settings and runtime-diagnostic fixes.

The RTX 2060 compatibility test has positive x2/x3/x4/x6, NR and loading-recovery
evidence. The combined release has positive ENB/RTX 4080 SUPER regression feedback
with x4/NR and loading recovery. RTX 20 support remains experimental; see
[compatibility limits](README.md#compatibility). NVIDIA runtimes and configuration
defaults are unchanged. HDR remains unsupported.

# 0.2.5

- Fixed startup compatibility with SSE Display Tweaks BorderlessUpscale and
  existing renderer hooks, including Community Shaders postprocessing.
- Save and Apply now show and log why a settings change was rejected.
- Fixed hidden Dynamic frame-generation settings blocking unrelated edits or
  resetting the selected multiplier.
- Kept NR's off control available when its requirements are unavailable, and
  allowed unchanged NR preferences to survive unrelated settings saves.
- Preserved explicit startup composition settings when saving other options,
  and clarified requested versus active upscaling settings in the log.
- Added clearer startup diagnostics when an observed NVIDIA App FG override
  prevents the configured frame-generation runtime from loading.

NVIDIA runtime DLLs and packaged defaults are unchanged. HDR remains unsupported
and RTX 30 compatibility remains experimental. The original remote DLAA reset
has not been reproduced; these changes are not claimed to establish its cause.

# 0.2.4

- Reorganized settings into Image, Neural Rendering, Frame generation and
  Advanced, with live FPS and related measurements beside their controls.
- Added a resizable window and draggable column divider. Save as default now
  remembers the window position, size and divider; the frame-time graph scales
  with the available space.
- Added independent input resolution, network preset and image tuning for the
  second NR pass, with linking and copying from Pass 1.
- Added descriptive DLSS preset labels and moved detailed runtime information
  behind Lab mode in Advanced. The live pipeline overview remains above the tabs.
- Disabled the NR bracket shortcuts by default to avoid shared-key conflicts.
  Set `EnableNRHotkeys=true` under `[Hotkeys]` to restore them; the NR menu
  checkbox remains available. Shortcut requests now appear in the log.
- Keep configured runtime paths portable when saving default settings.
- Preserve earlier renderer hook chains and check game-code patches before
  installation, with clear errors for incompatible or repeated device setup.

Existing NVIDIA runtime DLLs are unchanged. HDR remains unsupported and RTX 30
compatibility remains experimental. Temporal and bottleneck reuse are excluded.

# 0.2.3

- Fixed settings-menu keyboard input on tested Nolvus setups while retaining
  compatibility with LoreRim/KreatE.
- Added optional NR peripheral compression to reduce model pixel workload
  toward the screen edges.
- Added optional combined NR preparation passes. Both optimizations default off.
- Fixed corrupted percentage text in the NR description and DLAA tooltip.
- Fixed a renderer shutdown when NVIDIA reports a VRAM-budget warning after
  accepting frame-generation settings. Memory pressure and related hitches may
  still occur.

Temporal reuse is excluded. HDR remains unsupported; RTX 30 compatibility
remains experimental. Existing NVIDIA runtimes and default settings are retained,
with the two new NR options disabled by default.

# 0.2.2

- Enabled Neural Rendering with DLAA, before or after anti-aliasing.
- Improved experimental RTX 30-series compatibility in Universal, tested with
  Community Shaders on an RTX 3060 Laptop.
- Fixed the compatibility default when upgrading older configurations with a
  missing key, while respecting explicit opt-outs.
- Improved startup diagnostics for renderer edition, settings, GPU and
  initialization failures.

RTX 30 requires Universal; Standard can supply its NVIDIA runtimes. RTX 30
support remains experimental, with artifacting and intermittent hitches reported.
HDR remains unsupported. Existing features, packaged defaults and runtimes are
retained; the NR optimization experiments are not included.
