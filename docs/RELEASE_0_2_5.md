# 0.2.5 maintenance release

This release combines the startup hook compatibility, settings feedback and
NVIDIA runtime diagnostic fixes from PRs #44–46. It retains the existing menu,
NR controls, optional performance settings and NVIDIA runtimes.

## Changes

- Accept compatible SSE Display Tweaks BorderlessUpscale call hooks and forward
  existing E9/FF25 renderer entry hooks correctly, including CS postprocessing.
  Refused sites report their bytes, targets and module ownership.
- Show rejected Save/Apply errors before ordinary pending-change text, and log
  the reason. Inactive Dynamic targets no longer block unrelated changes or
  discard the selected frame-generation multiplier during sanitization.
- Keep NR's off action accessible when dedicated composition is unavailable.
  Unchanged NR preferences can be preserved during unrelated saves; newly
  enabling or modifying unavailable NR remains blocked.
- Preserve explicit on-disk startup composition settings when saving other
  options, including the legacy key migration. Log requested upscaling settings
  separately from the active allocation while a restart is pending.
- Explain a configured DLSS-G runtime retention failure when NVIDIA explicitly
  reports an FG override. Other generic runtime failures do not receive that
  diagnosis. Runtime identity and Ada/Ampere patch checks remain in place.

## Validation and limits

The merged source at `88d908e` has exactly the tested tree of `533ae94`. Both
editions passed 58 CTests and 30 offline SKSE Query cases, plus 64 checks against
retained game executables. Version preparation changes only release metadata
and documentation from that source.

The combined Universal candidate has positive Skyrim 1.6.1170 / RTX 4080 SUPER
feedback with ENB and Bottle CS 1.8.0. The ENB restart restored saved native DLAA,
sharpness 0.5 and NR input scale 0.905 with x4. CS installed the changed hook and
submitted frames with x4 and one/two-pass early NR; End open/close and session
Apply were logged. CS retains ownership of its own upscaling settings.

Both final sessions had zero TRP error entries; recurring vendor RSYNC errors
remain (two ENB, one CS), followed by continued generation. This is scoped game
acceptance, not proof of physical frame cadence or every mod combination.
Rejected-action footer appearance and unavailable-NR recovery have offline
coverage but have not been visually verified in-game. The original remote DLAA
reset remains unreproduced, and the remote NVIDIA override case needs a retest.
The final version-stamped DLLs have not been newly deployed or game-tested.

HDR remains unsupported and RTX 30 compatibility remains experimental. RTX 20
experiments, temporal/bottleneck reuse, kernel experiments and separate device-loss
diagnostics are excluded. Standard retains all eight NVIDIA runtime DLLs;
Universal retains Ada/Ampere compatibility and can use matching Standard's
runtimes when installed after it in MO2. Packaged defaults and attribution remain.
