# Weather and time presets

The **Presets** tab adjusts NR intensity, local tone, local structure and TRP
sharpening strength using Skyrim's weather and game clock. Automation defaults
off. ENB Helper is not required. Up to **512 named presets** and **4096 exact
weather assignments** are supported. Several groups and weathers can share one
preset; editing it updates every assignment.

## Layout

The top line shows **Automatic presets**, **Pause (use Base)** and what is applied
now: preset, time of day and weather. Hover it for the resolved values. During a
weather transition it also shows the outgoing preset and progress.

The list on the left starts with **Base**, then your presets, then **Timing**.
Each preset row says what it applies to; **NOW** marks the rows your edited
settings select for the current weather. Select a row to edit it on the right.

- **Base** holds the same values as the Neural Rendering and Image tabs; editing
  them here edits them there. Base applies wherever no preset does, and while
  automation is off or paused.
- **Timing** holds the time anchors and transition smoothing.

## Create and edit presets

1. Tune Base, or the normal NR and Image controls.
2. Select Base and choose **Copy into new preset**, or, with no presets yet,
   **Create starter presets** to make All exteriors, Clear, Cloudy, Rain, Snow and
   Interior presets from Base. These are editable starting points, not visually
   calibrated recommendations. **Copy into new preset** on any preset duplicates it.
3. Name the preset and choose whether it **Adjusts** NR, sharpening, or both.
   Clearing **Use this preset** keeps its assignments but lets them fall back.
4. Under **Applies to**, tick weather groups and add individual weathers.
5. Set its **Values**. With **Vary by time of day** off, one set of values applies
   all day. Turn it on to edit each of the six times separately; the current time
   is marked green. Turning it off keeps the selected time's values. **Reset to
   Base** (or **Set this time from Base**) copies the pending Base values, not the
   current blended output. Linked Pass 2 follows Pass 1.
6. Enable **Automatic presets**, then **Apply** for this session or **Save as
   default** for future sessions. **Discard** restores the applied configuration.

**Pause (use Base)** immediately restores normal NR/Image values. The pause is not
saved. Presets never enable NR or sharpening themselves. Community Shaders owns
sharpening on its route, so only TRP NR is automated there. Network preset, style,
skin controls, resolution, placement and pass count stay in the Neural Rendering
tab. See the [focused game check](APPEARANCE-TESTING.md) before relying on a new
preset set.

## Groups and weathers

Each weather group uses one preset; ticking a group on another preset moves it.
Weathers assigned to a preset show as chips; click a chip to remove it. **Add
current weather** is a shortcut for the incoming weather.

**Add weathers...** searches editor ID/name, owning plugin, FormID and weather
classification. Names use the engine or the optional Native EditorID Fix and
powerofthree's Tweaks public lookup APIs when loaded. Neither plugin is required;
a plugin/local FormID label remains available otherwise. **Refresh list** resamples
names on Skyrim's main thread. The list includes loaded records, not just weathers
that normally occur in the current region. Select rows individually or use **Select
shown** after filtering; selection survives filtering. Weathers assigned elsewhere
move to this preset. Capacity errors leave the batch unchanged.

Assignments for absent plugins remain saved as greyed chips; they do not match
another plugin with the same numeric ID. Deleting a preset removes its assignments
and returns its groups to less specific presets.

Exterior resolution is Base, then All exteriors, then Clear/Cloudy/Rain/Snow, then
an exact-weather assignment. Each preset can override NR and sharpening
independently. The top line names the source preset for each channel when they
differ. Fog, ash and special-worldspace weather can be assigned explicitly; their
visible appearance is not reliably described by the game's broad classification
alone. Ordinary interiors use only Interior over Base. Sky-lit interior cells
follow exterior rules.

## Time and transitions

Default anchors are Night 00:00, Dawn 05:00, Sunrise 07:00, Day 12:00, Sunset 18:00
and Dusk 20:00. Values interpolate between adjacent anchors, including midnight.
Hours must be within a 24-hour day and increase in that order.

**Copy ENB timing as anchors** reads `enbseries.ini` beside the game executable
only when clicked. It copies NightTime, SunriseTime, DayTime and SunsetTime and
derives Dawn as SunriseTime minus DawnDuration, Dusk as SunsetTime plus DuskDuration.
For example, markers 1/8.5/10.5/17.5 with durations 3.5/4 give anchors
01:00/05:00/08:30/10:30/17:30/21:30. Review and Apply the result. Missing or
non-increasing schedules leave the previous anchors unchanged.

This provides an editable approximation from the ENB configuration. TRP retains
its own linear interpolation; it does not reproduce ENB's internal phase weights,
follow later ENB edits automatically or change ENB settings.

Both outgoing and incoming presets are sampled at the current hour, then blended
using Skyrim's weather transition progress. Additional smoothing softens sudden
changes. Its seconds value is an exponential time constant, not a fixed completion
duration. Loading clears the old scene's blend. Zero smoothing follows the
weather/time result directly. Base values and authored presets never receive
intermediate blended values.

## Persistence and compatibility

Configuration remains in `Data/SKSE/Plugins/TheosRenderPipeline.ini`:

- `[Appearance]`: format 2, enable state, schedule, counts and smoothing.
- `[Appearance.Preset0]` etc.: stable ID, name, flags and six time subsections.
- `[Appearance.Clear]` etc.: the shared preset ID assigned to each broad group.
- `[Appearance.Weather0]` etc.: plugin name, hexadecimal local FormID and preset ID.

The original per-weather format from this PR migrates on load, retaining its
points and override flags. The next Save writes the shared format and removes
retired owned sections. Unrelated INI settings remain intact. Older plugin builds
cannot interpret shared format 2; keep a backup before reverting the DLL.

Install the matching `RCAS.hlsl` with the DLL. Sharpening uses a runtime constant
buffer; unchanged strength does not upload it again. Weather identities and preset
selection are cached across ordinary frames. Disabled automation samples the
context display at four updates per second.

Gradual automated tuning retains NR history. Manual/preset changes, loading,
camera resets, time jumps and discrete changes still reset it. Standalone tests
do not establish NVIDIA temporal image quality, game performance or visual
acceptance of the new UI.
