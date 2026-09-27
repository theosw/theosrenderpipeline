# Weather and time appearance profiles

The **Appearance** tab adjusts NR intensity, local tone, local structure and TRP
sharpening strength using Skyrim's weather and game clock. Automation defaults
off. ENB Helper is not required. Up to **512 named presets** and **4096 exact
weather assignments** are supported. Several groups and weathers can share one
preset; editing it updates every assignment.

## Start with your existing tuning

1. Tune the normal NR and Image controls, then open **Appearance**.
2. **Create missing starter templates** copies those edited controls to six
   presets: Exterior, Clear, Cloudy, Rain, Snow and Interior. It assigns missing
   broad groups without replacing existing assignments or enabling automation.
   These are editable starting points, not visually calibrated recommendations.
3. Select a preset under **Shared presets**, give it a useful name, and choose
   whether it overrides NR, sharpening, or both. **Duplicate** makes an independent
   copy. Disabled presets inherit less specific settings.
4. Adjust its six time points. **Copy this time to all** makes the preset constant
   through the day. Capture buttons copy the pending manual NR/Image controls,
   not the current blended output. Linked Pass 2 follows Pass 1.
5. Enable **Automatic appearance**, then **Apply** for this session or **Save as
   default** for future sessions. **Discard** restores the applied configuration.

**Use manual settings for this session** immediately restores normal NR/Image
values. The pause is not saved. Presets never enable NR or sharpening themselves.
Community Shaders owns sharpening on its route, so only TRP NR is automated there.
Network preset, style, skin controls, resolution, placement and pass count stay
manual. See the [focused game check](APPEARANCE-TESTING.md) before relying on a new
preset set.

## Find and assign weather

The loaded-weather list searches editor ID/name, owning plugin, FormID and weather
classification. Names use the engine or the optional Native EditorID Fix and
powerofthree's Tweaks public lookup APIs when loaded. Neither plugin is required;
a plugin/local FormID label remains available otherwise. **Refresh loaded weather list** resamples names
on Skyrim's main thread. The list includes loaded records, not just weathers that
normally occur in the current region.

Select rows individually or use **Select results** after filtering. Selection
survives filtering; its total is shown above the list. **Assign selected to edited
preset** shares the preset across those records. Capacity errors leave the batch
unchanged. **Remove selected assignments** restores inheritance. **Assign current
weather to edited preset** is a shortcut for the incoming weather.

**Assigned only** helps review existing mappings. Assignments for absent plugins
remain saved; they do not match another plugin with the same numeric ID. The
**Assignments for unavailable weathers** list can remove them. Deleting a preset
removes its assignments and returns its broad groups to inheritance.

Exterior resolution is manual defaults, then Exterior, then Clear/Cloudy/Rain/Snow,
then an exact-weather assignment. Each preset can override NR and sharpening
independently. The active panel names the source preset for each channel. Fog,
ash and special-worldspace weather can be assigned explicitly; their visible
appearance is not reliably described by the game's broad classification alone.
Ordinary interiors use only Interior over manual defaults. Sky-lit interior cells
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
weather/time result directly. Manual values and authored presets never receive
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
