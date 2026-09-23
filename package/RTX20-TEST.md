# RTX 20 compatibility — Universal 0.3.0

This combines the RTX 2060-tested compatibility path with released 0.2.5 fixes.
The combined build also has positive ENB/RTX 4080 SUPER regression feedback.
RTX 20 support remains experimental. Intended cards are RTX 2060, 2070 and
2080 variants; GTX 16 and other Turing products are excluded.

Keep Standard enabled for its runtimes (0.3.0 and 0.2.5 supply the same bundle).
Disable the previous Universal edition and enable Universal 0.3.0 below Standard
in MO2. Existing settings can stay; compatibility requires
`[Experimental] SourceDLSSGMFGUnlock=true`. Packaged defaults remain x2,
dynamic MFG off and NR off. No NVIDIA runtime replacement is part of this fix.

Use windowed/borderless mode with hardware-accelerated GPU scheduling. With
Community Shaders, disable its frame generation, Reflex and HDR. Keep other
upscaler/frame-generation injectors disabled. HDR remains unsupported.

1. With NR off, compare FG off/x2/x4 in the same scene and observe camera motion.
2. Play through a loading screen, open/close the menu and toggle FG off/on.
3. Save settings and restart once. Test NR separately afterward; it is expensive
   on the recorded RTX 2060 setup.

Send an End-menu screenshot and `TheosRenderPipeline.log` from
`Documents/My Games/Skyrim Special Edition/SKSE` before another launch overwrites
it. Include GPU/driver, ENB or CS and the settings used. On startup/kernel failure,
send the message and log rather than repeatedly reinstalling.

The log should select `route=Turing` and `stage=network-selection` with
`target=SM75 networks=2 kernelLoads=39 source=prepared-PTX`. This identifies
preparation; actual output and image quality are separate observations.

To roll back, disable Universal 0.3.0 and restore the previous setup. Standard
alone is not an RTX 20 FG fallback because TRP still requires its FG host at
startup. Details and prior RTX 2060 results are in the source repository's
`docs/RTX20_COMPATIBILITY.md`.
