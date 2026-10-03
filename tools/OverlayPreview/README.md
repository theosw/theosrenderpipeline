# Overlay preview

`TRPOverlayPreview` draws the in-game menu offline and writes PNGs, so menu
changes can be reviewed without launching Skyrim. It compiles the production
menu sources (`ARP_OVERLAY_MENU_SOURCES` and `OverlayNeuralPanel.cpp`) with
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
| `--full` | Keep the whole output rather than cropping to the menu |
| `--warp` | Use the WARP software rasterizer |
| `--out DIR` | Output folder |

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
menu, Skyrim's input routing, hover tooltips, popups, collapsed sections or
scrolled content, and the scenario values are illustrative rather than
measured. Check menu behaviour that depends on input or the renderer in game.
