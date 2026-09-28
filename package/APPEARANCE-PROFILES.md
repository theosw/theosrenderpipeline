# Weather and time presets

Presets live in the **Neural Rendering** tab. A preset changes any Neural
Rendering setting or sharpening for the weather or places you choose, using
Skyrim's weather and game clock. ENB Helper is not required. Up to **512 named
presets** and **4096 exact weather assignments** are supported.

## Layout

The left column lists **Base** first, then your presets. Base is your normal
Neural Rendering settings; selecting it shows the usual NR controls in Look,
Quality and Performance sections. Selecting a preset shows the same controls with
its changes marked. Each preset row says when it applies and how many settings it
changes, and **NOW** marks the rows your edited settings select for the current
weather. Once a preset exists, a **Now** line shows what is applied, the game time
and weather; hover it for the resolved values.

Presets apply whenever at least one preset is in use. There is no separate
automation switch. **Pause presets** immediately uses Base until you untick it or
restart the game; it is not saved.

## Create and edit presets

1. Tune Base. Sharpening is in Look and applies with or without NR.
2. **Create new preset** starts with no changes. **Duplicate** copies the selected
   preset.
3. Under **Use when**, tick weather types (Clear, Cloudy, Rain, Snow), Interior or
   Outdoors, and add specific weathers.
4. Change the settings you want different. A changed setting gets an amber mark and
   a **Base** button that returns it to Base's value; **Reset all to Base** clears
   every change. Settings you leave alone follow Base, including later Base edits.
5. **Apply** for this session or **Save as default** for future sessions.
   **Discard** restores the applied configuration.

Look settings (intensity, local tone, local structure and sharpening strength)
blend smoothly between weathers and times of day and keep NR history. With **Vary
by time of day** on, they can differ at each of the six times; the current time is
green. Turning it off keeps the selected time's values. Other settings switch once
at the midpoint of a weather change and reset NR history.

Settings marked **(!)** restart NR briefly when they change: placement, NR input
resolution, network preset, peripheral compression, combined preparation and
Reconstruction. A preset that changes one of them can hitch when the weather
changes; interior and exterior changes happen behind a loading screen. Pass count
switches without restarting NR: two passes stay allocated while any preset uses
them, and one-pass situations skip Pass 2 like the combat option.

Presets never switch NR or sharpening on while Base has them off, but can switch
them off. The Image tab shows Base sharpening read-only, and the value in use when
a preset changes it. Community Shaders owns sharpening on its route, so presets
change only TRP NR there. **More options** can ignore a preset without deleting it
(**Use this preset**). **Timing...** holds the times of day and transition
smoothing. See the [focused game check](APPEARANCE-TESTING.md) before relying on a
new preset set.

## Weather types and specific weathers

Each weather type, Interior and Outdoors uses one preset; ticking one on another
preset moves it. Specific weathers show as chips; click a chip to remove it.
**Add current weather** is a shortcut for the incoming weather.

**Add weathers...** searches editor ID/name, owning plugin, FormID and weather
type. Names use the engine or the optional Native EditorID Fix and powerofthree's
Tweaks public lookup APIs when loaded. Neither plugin is required; a plugin/local
FormID label remains available otherwise. **Refresh list** resamples names on
Skyrim's main thread. The list includes loaded records, not just weathers that
normally occur in the current region. Select rows individually or use **Select
shown** after filtering; selection survives filtering. Weathers assigned elsewhere
move to this preset. Capacity errors leave the batch unchanged.

Assignments for absent plugins remain saved as greyed chips; they do not match
another plugin with the same numeric ID. Deleting a preset removes its assignments
and returns its weather types to less specific presets.

Outdoor resolution is Base, then Outdoors, then Clear/Cloudy/Rain/Snow, then a
specific weather, per setting: a more specific preset overrides only the settings
it changes. The Now line names every preset in use, most specific first. Fog, ash and
special-worldspace weather can be added as specific weathers; their visible
appearance is not reliably described by the game's broad classification alone.
Ordinary interiors use only Interior over Base. Sky-lit interior cells follow
outdoor rules.

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

- `[Appearance]`: format 3, schedule, counts and smoothing. `Enabled` is written
  as whether any preset is in use, for older builds; this build derives it on load.
- `[Appearance.Preset0]` etc.: stable ID, name, enable state and each changed
  setting other than look settings, such as `Passes = 2`.
- `[Appearance.Preset0.Day]` etc.: changed look settings at each time of day.
- `[Appearance.Clear]` etc.: the shared preset ID assigned to each broad group.
- `[Appearance.Weather0]` etc.: plugin name, hexadecimal local FormID and preset ID.

Only changed settings are saved. Format 1 and 2 presets migrate on load: their
intensity, tone, structure and sharpness at each time become look changes, limited
to the channels they changed. The next Save writes format 3 and removes retired
owned sections. Unrelated INI settings remain intact. Older plugin builds cannot
interpret format 3; keep a backup before reverting the DLL.

Install the matching `RCAS.hlsl` with the DLL. Sharpening uses a runtime constant
buffer; unchanged strength does not upload it again. Weather identities and preset
selection are cached across ordinary frames. Disabled automation samples the
context display at four updates per second.

Gradual look changes retain NR history. Manual/preset edits, loading, camera
resets, time jumps and other setting changes still reset it. Standalone tests
do not establish NVIDIA temporal image quality, game performance or visual
acceptance of the new UI.
