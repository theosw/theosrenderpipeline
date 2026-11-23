# Overlay preview

`TRPOverlayPreview` draws the in-game menu offline, either to PNGs or live in
a window, so menu changes can be reviewed without launching Skyrim. It
compiles the production menu sources (`ARP_OVERLAY_MENU_SOURCES` and
`OverlayNeuralPanel.cpp`) with
the same style, embedded font, layout and zoom rules as the plugin. Only the
renderer state is invented: each scenario fills `OverlayUI::FrameView` and the
settings draft in `PreviewScenarios.cpp`.

It is built with the compatibility tests (the public presets enable them):

```powershell
cmake --build out/build/base --config Release --target TRPOverlayPreview
out/build/base/Release/TRPOverlayPreview.exe --list
out/build/base/Release/TRPOverlayPreview.exe --scenario healthy,presets --tab neural --size 1920x1080,2560x1440
```

Images go to `out/overlay-preview/<scenario>-<tab>-<width>x<height>.png`,
cropped to the menu window. Options:

| Option | Effect |
| --- | --- |
| `--scenario A,B` | Scenarios to draw; default all |
| `--tab A,B` | `image`, `neural`, `generation`, `advanced`; default all |
| `--size WxH,...` | Game output sizes; default 1920x1080 |
| `--zoom PERCENT` | Fixed menu zoom; default automatic, as in game |
| `--menu WxH` | Menu window size at 100% zoom, for narrow or tall layouts |
| `--ini PATH` | Read the `[Overlay]` layout from a TheosRenderPipeline.ini |
| `--language CODE` | Overlay language: `auto` (default), `en`, or a dictionary code |
| `--lang-dir DIR` | Read language files from `DIR`. The default is this checkout's `package/SKSE/Plugins/TheosRenderPipeline/Lang`, read in place, so an edited file shows the next time its language is picked in `--live` |
| `--full` | Keep the whole output rather than cropping to the menu |
| `--support` | Also write `<scenario>-support.txt`: the text the Advanced tab's Copy details button would copy |
| `--warp` | Use the WARP software rasterizer |
| `--out DIR` | Output folder |

## Live window

`--live` opens a window with the menu on the right and a sidebar on the left.
The menu works as in game: tabs, combos, sliders, tooltips, popups, presets,
zoom, and Apply, which in the preview updates the scenario's applied settings
so the pending-change flow can be tried. The sidebar switches scenarios,
the game output size (fitted to the window or pixel for pixel) and the
renderer state the menu reads (Lab mode, Community Shaders, NVIDIA GPU, NR
runtime, NR state, frame generation and provider), and saves a PNG of the
menu. Renderer state does not follow Apply as it would in game.

```powershell
out/build/base/Release/TRPOverlayPreview.exe --live --scenario presets --size 2560x1440
```

A scripted run clicks and hovers without a mouse, then saves and exits. The
positions are output pixels from the menu window's top-left, as in the cropped
PNGs; the hover rests long enough for tooltips, and `--full` keeps tooltips
that extend past the menu:

```powershell
out/build/base/Release/TRPOverlayPreview.exe --live --scenario healthy `
    --click 132,144 --click 975,373 --hover 920,270 --save out/overlay-preview/pass2-off.png
```

`--type TEXT` types into the focused field, one character per frame;
`{enter}` presses Enter. For example, `--click 1540,322 --type "7{enter}"`
sets a custom NR style.

The `OverlayPreview` CTest renders every scenario and tab with WARP, so a
menu change that no longer builds or draws offline fails the suite.

## Keeping the menu drawable offline

Menu drawing reads the renderer only through `FrameView`, which
`CaptureFrameView` (`OverlayFrameCapture.cpp`) fills once per menu frame.
Actions that change the renderer go through the `OverlayUI` members declared
with `CaptureFrameView`; the plugin defines them in `OverlayUI.cpp`, the
preview in `PreviewHost.cpp`. To show new renderer state, add a `FrameView`
field, capture it, and set it in the scenarios that need it. Lab-only details
that read renderer internals belong in `OverlayLabPanel.cpp`; the preview
draws a note in their place.

## Limits

A preview is not game acceptance. It does not show the game frame behind the
menu or Skyrim's input routing, and the scenario values are illustrative
rather than measured. Offline PNGs also omit tooltips, popups and scrolled
content; use the live window or a scripted run for those. Check menu
behaviour that depends on input or the renderer in game.
