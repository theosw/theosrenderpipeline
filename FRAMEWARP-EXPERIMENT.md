# Experimental native camera reprojection

This is an optional research build of XPAR's own camera reprojection engine.
It is off in normal builds and off at startup. It has not been accepted in Skyrim.
It does not generate animated object frames, replace TRP's FG/MFG providers,
or initialize NVIDIA Reflex 2/Latewarp. ReShade is not required.

## Build and layout

```powershell
cmake --preset trp-standard -DTRP_ENABLE_FRAMEWARP_EXPERIMENT=ON
cmake --build out/build/base --config Release
ctest --preset trp-standard
```

The Universal preset also supports the option. Both editions retain NR.
The helper is emitted in `out/build/<edition>/Release/FrameWarp/`.
An experimental installation needs that executable at
`Data/SKSE/Plugins/TheosRenderPipeline/FrameWarp/TRPFrameWarpPresenter.exe`
and this experiment's matching renderer DLL. Include the upstream GPL license
and attribution when distributing the helper. CMake does not install or deploy it.
No extra proprietary runtime is needed by the helper's own engine.

## Controls and initial scope

Under the developer Lab controls in Frame generation, turn on **Experimental
camera reprojection**. This setting lasts for the current session only. Close
settings to start. **FrameWarp A/B: original image** keeps the helper's presentation
path but disables the warp. Disabling the experiment exposes TRP's original
native presentation path again.

The initial input contract requires:

- Frame generation explicitly off, including a request still waiting for warmup.
- SDR output and matching `R8G8B8A8_UNORM` completed color, HUD-less color, and UI.
- Dedicated native UI and a fresh valid world camera.
- Matching `R32_FLOAT` depth and `R16G16_FLOAT` motion guides at render resolution.

If XPAR rejects the native camera/motion agreement, the bridge suspends until host
retirement or restart. It does not enter the estimated-camera fallback.

HDR, unsupported formats/devices, paused menus, loading transitions, and the TRP
settings overlay suspend the helper. Native rendering continues. A producer
fault stays latched until successful host retirement (such as resize) or restart.
A missing helper cannot fault the main renderer.

## Ownership and timing

TRP captures after world rendering, the final UI copy, late NR, and native output
composition. Each accepted source contains the completed backbuffer, HUD-less
image, premultiplied UI, depth, motion, and camera in one immutable slot.
There is no interception of eager Streamline tags.

Four named private texture sets carry images to the helper. A slot follows
Free → Writing → Ready → Reading → Free. TRP publishes Ready only after its
output list is submitted and its capture fence is successfully signalled. The
helper chooses sources whose copies are already complete, imports them on the
same adapter, and returns Reading slots only after its GPU intake retires.
TRP also checks its previous copy fence before reusing a slot. Busy slots cause
a dropped capture, with no producer CPU wait. Fence/device failures retain GPU
owned resources; host queue retirement is required before destruction.

The Lab panel reports completed GPU timestamps for the native copies, separately
from the helper's warp timing. Pending/unavailable samples are labelled. Averages
and maxima cover captured frames, not dropped captures or full render latency.

The camera timestamp records when the native frame's constants reach Prepare.
A simulation-start timestamp is unavailable and remains zero. The helper learns
raw mouse/controller response from camera history; its age and latency counters
are model/queue estimates, not physical input-to-photon measurements. Idle
telemetry can show the last sample. The engine's processing cost and native-copy
cost must be measured at the intended resolution rather than assumed negligible.

## Acceptance still required

Standalone fixtures verify transport, camera math, marker displacement, UI
position, and retirement. They cannot establish Skyrim image quality or display
cadence. A later requested game test should compare matched scenes with FG off
and ReShade disabled: native presentation, helper original-image mode, and warp.
Measure CPU/GPU frame tails, presenter intake and warp cost, capture drops,
VRAM use, and physical output cadence separately. Check first/third person,
weapons, moving objects, disocclusions, mouse/controller, menu/loading transitions,
alt-tab, resize, and helper failure.

The overlay is composed by Windows at display refresh; VRR behavior differs from
a game's direct presentation. Original-image mode controls the warp itself;
helper disabled versus enabled controls the extra presentation path. Neither an
output FPS counter nor a successful hidden-window fixture proves lower physical
latency. Combining this with FG/MFG remains outside this first experiment.

Upstream revision, license, and local changes: [FrameWarp subset](extern/framewarp/README.md).
