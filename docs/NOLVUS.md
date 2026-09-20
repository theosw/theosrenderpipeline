# Nolvus Awakening setup and test status

Nolvus uses the normal Standard and Universal editions. It does not need a
separate renderer. This branch adds a keyboard-input fallback for installations
where Skyrim receives the menu key but Windows does not deliver it to TRP.
The fallback is in [PR #33](https://github.com/theosw/theosrenderpipeline/pull/33);
it is not included in the original 0.2.2 release ZIPs.

## Install

1. Make a copy of your Nolvus MO2 profile for testing and close Skyrim before
   changing mods. Keep the list's matching SKSE and Address Library versions.
2. Install Standard for the NVIDIA runtimes. For RTX 30 compatibility or RTX 40
   multipliers above x2, install matching Universal below Standard in MO2's left
   pane and let Universal win conflicts. RTX 30 requires Universal even with
   frame generation switched off. Leave `SourceDLSSGMFGUnlock=true`.
3. If using the separate Nolvus Input Test ZIP, place it below both editions.
   It replaces only the renderer DLL and inherits your settings and runtimes.
   Keep the underlying installation enabled. Disable the test overlay after
   closing Skyrim to return to your previous renderer.
4. Disable competing upscaler/anti-aliasing and frame-generation components.
   In Awakening, check **ENB Anti-Aliasing**, **ENB Frame Generation**, and its
   dedicated **Settings** and **DisplayTweaks Settings** mods, where present.
   Keep the base SSE Display Tweaks mod; disable the settings override supplied
   specifically for ENB frame generation. Also disable other upscalers or
   frame-generation injectors you added yourself.
5. Keep the list's ENB and ReShade preset. Disable **SSE ReShade Helper** if
   installed; TRP supplies the ReShade stages. Use windowed/borderless mode,
   Hardware-Accelerated GPU Scheduling, and SDR. HDR is not supported.
6. Give TRP its own menu key. **End** conflicts with STB Active Effects in the
   tested profile. F10 worked there; first check that your other mods do not use
   it. In the winning `SKSE/Plugins/TheosRenderPipeline.ini`, set:

   ```ini
   [Hotkeys]
   ToggleOverlay=0x79
   ```

   MO2's Data tab identifies which mod supplies the winning INI; an Overwrite
   copy can override the settings in either edition. Preserve the rest of the
   file. Restart Skyrim after changing this binding. Click outside an active
   text field before using F10 to close the menu.
7. Launch SKSE through the Nolvus MO2 instance. Start with NR off and check that
   the menu opens and closes before changing rendering settings.

Mod names and available components vary by Nolvus version and install options.
The [official Awakening list](https://www.nolvus.net/awakening) lists its ENB
anti-aliasing and frame-generation options. This guidance covers the ENB setup;
for a separately modified CS setup, also follow [Community Shaders setup](COMMUNITY_SHADERS.md).

## What has been checked

The local test used **Nolvus Awakening 6.0.20, Skyrim 1.5.97, ENB 0.504,
ReShade 6.3.1, RTX 4080 SUPER and 5120x1440 output**. Universal candidate
`f078b1b` reached gameplay with DLSS Quality, x2 and NR off. The user confirmed
the F10 menu was visible; the log recorded three open/close pairs.

This is limited candidate evidence, not acceptance of every Nolvus option.
Short/held-key matching, closing after editing, restored movement, focus
recovery, x4, NR in both placements, and Standard gameplay still need checks.
Runtime output counts do not establish smooth displayed frame spacing.

Early OAR and IED loading panels were reported at low resolution. Their installed
DLL versions are not recognised by the native UI adapters. The same rejection
exists in the pre-fix log, so it is a separate limitation of this setup.

## Focused test

- With other overlays closed, tap F10 to open and close TRP several times. Hold
  it briefly: one press should cause one toggle, without repeated opening/closing.
- Edit a numeric setting, click outside the field, close with F10, and check
  movement and mouse look. Alt-Tab away and back; a background F10 press should
  not change the menu when you return.
- With NR off, compare x2 and x4 where the edition/GPU supports them. Check
  motion and UI, not only the displayed FPS number.
- Enable NR with one pass before DLSS, then after DLSS in the same scene. Use
  Apply now for the comparison. Check sky colours, grass edges and UI. Lower NR
  input resolution if needed; record it so the comparison is interpretable.
- Open and close inventory and a menu with a character/item preview, then make
  one ordinary cell transition. Report any black background, stuck input, or
  incorrectly scaled UI separately from overall gameplay feedback.

Keep your existing saves; no automatic save loading is needed. Include the
candidate/package identity, Nolvus version, GPU, settings and
`TheosRenderPipeline.log` with results. Logs are normally in
`Documents/My Games/Skyrim Special Edition/SKSE/`; copy them before another launch.
