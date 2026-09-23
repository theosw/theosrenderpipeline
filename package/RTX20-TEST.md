# RTX 20 PTX network test — Universal 0.2.4

This is a separate compatibility candidate, not confirmed RTX 20 support.
The previous RTX 2060 diagnostic identified a precompiled RTX 30 kernel being
selected on RTX 20. This build selects the matching PTX network implementations
whose GPU programs we already convert to SM75. It keeps the synchronous error
guard if NVIDIA still rejects a kernel. No NVIDIA runtime files are replaced.
It includes the 0.2.4 menu and local startup/settings hardening. Its Turing
frame-generation path has offline validation; this correction needs a fresh
RTX 20 test. Driver loading on an RTX 4080 SUPER does not establish RTX20 support.
The intended cards are RTX 2060, 2070 and 2080, including Super, Ti and laptop
variants. GTX 16 cards and other Turing products are excluded from this test.

## Install

1. Keep the regular **Standard 0.2.3 or 0.2.4** enabled for its NVIDIA runtimes.
   Install this ZIP as a separate MO2 mod named
   **Theo's Render Pipeline - Universal Turing PTX Test**. Enable it below Standard
   in the left pane so this test DLL and INI win. Disable any previous Universal
   edition. If MO2 proposes merging/replacing the old mod, cancel and give the
   test its separate name.
2. Back up existing TRP settings. Check that an older TRP INI in Overwrite or
   another mod is not overriding the test's INI. Keep
   `[Experimental] SourceDLSSGMFGUnlock=true`.
3. Use windowed/borderless mode and hardware-accelerated GPU scheduling.
   With Community Shaders, disable **its frame generation, Reflex and HDR**.
   Disable other upscaler/frame-generation injectors. HDR remains unsupported.
4. Start with **frame generation x2, dynamic MFG off and NR off**. Those are the
   supplied defaults. The first launch may take longer while the driver compiles
   GPU programs; make a note if startup fails or stalls.

## Short test

For the first test, keep NR off and FG at x2. Load a save once. If a
kernel-loading error appears, close the message and send the log before another
launch overwrites it. Look for `stage=cu-module-load`. Do not repeat quality-mode
tests or reinstall. The instructions below only apply if kernel loading succeeds.

- Record GPU model, driver, game version and ENB/Community Shaders version.
- In a repeatable scene, compare FG off and x2 with NR off. Observe actual motion
  and responsiveness as well as base/output FPS. A multiplied counter alone
  does not establish smooth presentation.
- If x2 works, try x3 and x4 in the same scene. Open/close a menu and toggle FG
  off/on. Note new artifacts, hitches, freezes or crashes.
- Save and restart once to check startup and settings persistence.
- Only then try NR separately at a modest input resolution, one pass. Compare
  before/after placement if available. NR can be expensive on these cards.
- If anything fails, capture the message and send the log before the next launch
  overwrites it. Do not reinstall just to recover the log.

Send `Documents/My Games/Skyrim Special Edition/SKSE/TheosRenderPipeline.log`
and this ZIP's `TRP-FULL-PACKAGE.txt`, plus an End-menu screenshot and the
settings/stage where the problem occurred. Successful startup should select
`route=Turing`, preparation for `SM75`, and `stage=network-selection` with
`networks=2 kernelLoads=39 source=prepared-PTX`. That startup line confirms the
selected code path; it does not by itself prove successful frame generation.

To roll back, disable this test mod, restore the backed-up INI and enable your
previous Universal edition. Standard alone is not an RTX 20 fallback for TRP:
the current renderer still requires its frame-generation host at startup.

## What remains unknown

Offline checks cover program conversion/assembly, guarded publication/rollback,
driver/OS error preservation, DLSS isolation and repeated FG creation with
vendor doubles. They do not prove real RTX 20 inference, image quality,
performance or displayed frame cadence. FP16 matrix accumulation differs from
the original Ada instructions, so image quality needs inspection too.
